// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBDragonBreath.h"

#include "Emberfall.h"
#include "EMBDragonAIController.h"
#include "EMBDragonBoss.h"
#include "EMBGameplayTags.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"

namespace
{
	/** Vitesse de montée du dragon pendant le télégraphe des modes aériens (cm/s). */
	constexpr float HoverClimbSpeed = 1500.f;

	/** Vitesse de rotation du dragon vers l'angle de départ du balayage (degrés/s). */
	constexpr float SweepAlignSpeed = 120.f;

	/** Longueur de trace du faisceau aérien vers le sol (cm). */
	constexpr float AerialBeamTraceLength = 4000.f;

	/** Un tick de zone de feu tous les N ticks de souffle (8 ticks/s : une zone toutes les 0,375 s). */
	constexpr int32 AerialZoneEveryNTicks = 3;

	/** Garde-fou : nombre maximal de ticks de souffle rattrapés en une seule frame (hitch). */
	constexpr int32 MaxCatchUpTicksPerFrame = 4;

	/** Longueur minimale de la ligne de strafe aérien (cm) et dépassement derrière la cible. */
	constexpr float MinStrafeLength = 3000.f;
	constexpr float StrafeOvershoot = 1500.f;
}

UBTTask_EMBDragonBreath::UBTTask_EMBDragonBreath()
{
	NodeName = TEXT("EMB Dragon Breath");

	// Une instance par arbre (état propre à l'exécution), tick activé, notification de fin pour le nettoyage.
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

void UBTTask_EMBDragonBreath::ResetState()
{
	BossPtr.Reset();
	TargetPtr.Reset();
	ActiveRow = nullptr;
	Stage = EBreathStage::Telegraph;
	Mode = EEMBBreathMode::Cone;
	StageTime = 0.f;
	TelegraphDuration = 0.f;
	FireDuration = 0.f;
	AimLockTime = 0.f;
	TickAccumulator = 0.f;
	FireTickCount = 0;
	bNeedsFlight = false;
	bAimLocked = false;
	bCleanedUp = false;
	ClimbTargetZ = 0.f;
	BeamPitchDeg = 0.f;
	SweepStartYaw = 0.f;
	SweepSign = 1.f;
	StrafeEnd = FVector::ZeroVector;
	StrafeSpeed = 0.f;
	TrailZonePositions.Reset();
}

EBTNodeResult::Type UBTTask_EMBDragonBreath::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBDragonBoss* Boss = AIController ? Cast<AEMBDragonBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	FGameplayTag AttackTag;
	if (bUseCataclysmTag)
	{
		AttackTag = EMBTags::Attack_Dragon_Cataclysm.GetTag();
	}
	else
	{
		AttackTag = FGameplayTag::RequestGameplayTag(Blackboard->GetValueAsName(AEMBDragonAIController::BBKey_SelectedAttackTag), false);
	}

	const FEMBDragonAttackRow* Row = Boss->FindAttackRow(AttackTag);
	if (!Row || (Row->Kind != EEMBDragonAttackKind::Breath && Row->Kind != EEMBDragonAttackKind::Scripted))
	{
		return EBTNodeResult::Failed;
	}

	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBDragonAIController::BBKey_TargetActor));
	if (!Target && Row->BreathMode != EEMBBreathMode::Cataclysm)
	{
		return EBTNodeResult::Failed; // un souffle dirigé exige une cible
	}

	// --- Initialisation (aucun effet de bord avant ce point) ---
	// Annule d'abord toute attaque programmée par le boss (impacts de mêlée, braises) qui serait encore en cours.
	Boss->CancelTimedAttack(true);
	ResetState();
	BossPtr = Boss;
	TargetPtr = Target;
	ActiveRow = Row;
	Mode = Row->BreathMode;
	bNeedsFlight = (Mode == EEMBBreathMode::Aerial || Mode == EEMBBreathMode::Cataclysm);

	// Le Cataclysm garde ses 4 s de charge : les télégraphes enragés (x0,9) ne le raccourcissent pas.
	const float TelegraphMultiplier = (Mode == EEMBBreathMode::Cataclysm) ? 1.f : Boss->GetTelegraphMultiplier();
	TelegraphDuration = Row->TelegraphTime * TelegraphMultiplier;

	switch (Mode)
	{
	case EEMBBreathMode::Cone:
		FireDuration = Row->ActiveDuration > 0.f ? Row->ActiveDuration : EMBDragonDefaults::ConeBreathDuration;
		// Verrouillage du cône au début de la fenêtre d'esquive (1,4 s sur 1,6 s) : le joueur lit la direction définitive.
		AimLockTime = Row->DodgeWindowStart > 0.f ? Row->DodgeWindowStart * TelegraphMultiplier : TelegraphDuration * 0.85f;
		AimLockTime = FMath::Min(AimLockTime, TelegraphDuration);
		break;
	case EEMBBreathMode::Sweep:
		FireDuration = Row->ActiveDuration > 0.f ? Row->ActiveDuration : EMBDragonDefaults::SweepBreathDuration;
		break;
	case EEMBBreathMode::Aerial:
		FireDuration = Row->ActiveDuration > 0.f ? Row->ActiveDuration : EMBDragonDefaults::AerialStrafeDuration;
		break;
	case EEMBBreathMode::Cataclysm:
		FireDuration = EMBDragonDefaults::CataclysmAfterglow;
		break;
	}

	Boss->Multicast_Telegraph(AttackTag, TelegraphDuration);
	if (UAnimMontage* Montage = Boss->ResolveMontage(Row->Montage))
	{
		Boss->PlayAttackMontage(Montage, Boss->GetAttackPlayRate());
	}

	if (bNeedsFlight)
	{
		Boss->SetFlying(true);
		const float Altitude = (Mode == EEMBBreathMode::Aerial) ? EMBDragonDefaults::AerialStrafeAltitude : EMBDragonDefaults::CataclysmAltitude;
		ClimbTargetZ = static_cast<float>(Boss->GetActorLocation().Z) + Altitude;
	}

	if (Mode == EEMBBreathMode::Sweep)
	{
		// Le balayage passe par la direction de la cible au moment du télégraphe, dans un sens tiré au hasard.
		// Le dragon n'est pas suivi par le regard pendant qu'il s'aligne sur l'angle de départ.
		SweepSign = FMath::RandBool() ? 1.f : -1.f;
		const float AimYaw = static_cast<float>((Target->GetActorLocation() - Boss->GetActorLocation()).Rotation().Yaw);
		SweepStartYaw = AimYaw - SweepSign * EMBDragonDefaults::SweepArcDeg * 0.5f;
		Boss->SetFacingLocked(true);
	}
	else
	{
		Boss->SetFacingLocked(false); // suit la cible jusqu'au verrouillage
	}

	Stage = EBreathStage::Telegraph;
	return EBTNodeResult::InProgress;
}

void UBTTask_EMBDragonBreath::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBDragonBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || !ActiveRow)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	StageTime += DeltaSeconds;

	if (Stage == EBreathStage::Telegraph)
	{
		if (bNeedsFlight)
		{
			bool bBlocked = false;
			const FVector Location = Boss->GetActorLocation();
			Boss->StepFlightToward(FVector(Location.X, Location.Y, ClimbTargetZ), HoverClimbSpeed, DeltaSeconds, bBlocked);
		}

		if (Mode == EEMBBreathMode::Sweep)
		{
			Boss->RotateYawToward(SweepStartYaw, DeltaSeconds, SweepAlignSpeed);
		}
		else if (Mode == EEMBBreathMode::Cone && !bAimLocked && StageTime >= AimLockTime)
		{
			LockAim(*Boss);
		}

		if (StageTime >= TelegraphDuration)
		{
			BeginFiring(*Boss);
		}
		return;
	}

	// --- Phase de tir ---
	if (Mode == EEMBBreathMode::Sweep)
	{
		// Rotation progressive : l'angle suit le temps écoulé (indépendant du nombre de ticks de dégâts).
		const float Alpha = FMath::Clamp(StageTime / FMath::Max(FireDuration, KINDA_SMALL_NUMBER), 0.f, 1.f);
		const float Yaw = SweepStartYaw + SweepSign * EMBDragonDefaults::SweepArcDeg * Alpha;
		Boss->SetActorRotation(FRotator(0.f, Yaw, 0.f));
	}
	else if (Mode == EEMBBreathMode::Aerial)
	{
		bool bBlocked = false;
		Boss->StepFlightToward(StrafeEnd, StrafeSpeed, DeltaSeconds, bBlocked);
	}

	if (Mode != EEMBBreathMode::Cataclysm)
	{
		// Ticks de dégâts à cadence fixe (8/s) découplée du framerate. Le rattrapage est plafonné pour qu'un
		// hitch serveur ne déclenche pas une rafale de dégâts d'un coup.
		const float TickStep = 1.f / EMBDragonDefaults::BreathTicksPerSecond;
		TickAccumulator += DeltaSeconds;
		int32 Iterations = 0;
		while (TickAccumulator >= TickStep && Iterations < MaxCatchUpTicksPerFrame)
		{
			TickAccumulator -= TickStep;
			++Iterations;
			DoBreathTick(*Boss);
		}
		TickAccumulator = FMath::Min(TickAccumulator, TickStep);
	}

	if (StageTime >= FireDuration)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

void UBTTask_EMBDragonBreath::LockAim(AEMBDragonBoss& Boss)
{
	bAimLocked = true;
	Boss.SetFacingLocked(true);

	// Inclinaison du faisceau figée vers la position de la cible à cet instant (le yaw est celui du dragon).
	const AActor* Target = TargetPtr.Get();
	const FVector Origin = Boss.GetBreathOrigin();
	const FVector AimPoint = Target ? Target->GetActorLocation() : Boss.GetActorLocation() + Boss.GetActorForwardVector() * 1500.f;
	const FVector Delta = AimPoint - Origin;
	const float RawPitchDeg = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(Delta.Z, Delta.Size2D())));
	BeamPitchDeg = FMath::Clamp(RawPitchDeg, -60.f, 10.f);
}

void UBTTask_EMBDragonBreath::BeginFiring(AEMBDragonBoss& Boss)
{
	Stage = EBreathStage::Firing;
	StageTime = 0.f;
	TickAccumulator = 0.f;
	FireTickCount = 0;

	const FEMBDragonAttackRow& Row = *ActiveRow;

	switch (Mode)
	{
	case EEMBBreathMode::Cone:
		if (!bAimLocked)
		{
			LockAim(Boss);
		}
		break;

	case EEMBBreathMode::Sweep:
		LockAim(Boss); // fige l'inclinaison ; le yaw est piloté par la rotation du balayage
		break;

	case EEMBBreathMode::Aerial:
	{
		// Ligne de strafe : du dragon vers la cible puis au-delà, à altitude constante, durée = ActiveDuration.
		const AActor* Target = TargetPtr.Get();
		const FVector Start = Boss.GetActorLocation();
		FVector Direction = Target ? (Target->GetActorLocation() - Start).GetSafeNormal2D() : Boss.GetActorForwardVector().GetSafeNormal2D();
		if (Direction.IsNearlyZero())
		{
			Direction = Boss.GetActorForwardVector().GetSafeNormal2D();
		}
		const float DistanceToTarget = Target ? static_cast<float>(FVector::Dist2D(Start, Target->GetActorLocation())) : 0.f;
		const float MaxLength = EMBDragonDefaults::FlySpeed * FireDuration;
		const float Length = FMath::Clamp(DistanceToTarget + StrafeOvershoot, MinStrafeLength, MaxLength);

		StrafeEnd = Start + Direction * Length;
		StrafeSpeed = Length / FMath::Max(FireDuration, KINDA_SMALL_NUMBER);

		Boss.SetActorRotation(FRotator(0.f, Direction.Rotation().Yaw, 0.f));
		Boss.SetFacingLocked(true);
		break;
	}

	case EEMBBreathMode::Cataclysm:
	{
		// Embrasement total instantané : la ligne de vue est testée depuis la gueule pour chaque joueur
		// (un pilier ou un mur de l'arène protège). Le prochain Cataclysm est reprogrammé ici (et non au
		// lancement) : un Cataclysm interrompu pendant sa charge est rejoué dès la fin de l'étourdissement.
		Boss.ApplyAreaDamage(Boss.GetBreathOrigin(), EMBDragonDefaults::CataclysmRadius, Boss.MakeHitData(Row), true);
		Boss.NotifyCataclysmFired();
		break;
	}
	}

	// VFX de souffle attaché à la gueule, arrêté dans Cleanup() (fin normale ou abandon).
	if (UNiagaraSystem* System = Boss.ResolveVFX(Row.VFX))
	{
		Boss.Multicast_StartBreathVFX(System, NAME_None);
	}
}

void UBTTask_EMBDragonBreath::DoBreathTick(AEMBDragonBoss& Boss)
{
	const FEMBDragonAttackRow& Row = *ActiveRow;
	const FEMBHitData Template = Boss.MakeHitData(Row);
	const FVector Origin = Boss.GetBreathOrigin();
	const float Yaw = static_cast<float>(Boss.GetActorRotation().Yaw);

	TArray<AActor*> Landed;

	switch (Mode)
	{
	case EEMBBreathMode::Cone:
	case EEMBBreathMode::Sweep:
	{
		const FVector Direction = FRotator(BeamPitchDeg, Yaw, 0.f).Vector();
		Landed = Boss.ApplyConeDamage(Origin, Direction, EMBDragonDefaults::BeamFullAngleDeg * 0.5f, Row.MaxRange, Template);
		break;
	}

	case EEMBBreathMode::Aerial:
	{
		// Faisceau incliné vers le sol : on cherche son point d'impact et on y applique une zone de dégâts.
		const FVector Direction = FRotator(-EMBDragonDefaults::AerialBeamPitchDeg, Yaw, 0.f).Vector();
		const FVector BeamEnd = Origin + Direction * AerialBeamTraceLength;

		FVector Impact = BeamEnd;
		if (const UWorld* World = Boss.GetWorld())
		{
			FHitResult BeamHit;
			FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBDragonAerialBeam), false, &Boss);
			Impact = World->LineTraceSingleByChannel(BeamHit, Origin, BeamEnd, ECC_Visibility, Params)
				? BeamHit.ImpactPoint
				: Boss.ProjectToGround(BeamEnd);
		}

		const float Radius = Row.EffectRadius > 0.f ? Row.EffectRadius : EMBDragonDefaults::AerialZoneRadius;
		Landed = Boss.ApplyAreaDamage(Impact, Radius, Template);

		// Laisse une zone de feu persistante (8 s, 20/s) derrière le passage du faisceau.
		if (FireTickCount % AerialZoneEveryNTicks == 0)
		{
			Boss.AddFireZone(Impact, Radius, EMBDragonDefaults::AerialZoneDuration, EMBDragonDefaults::AerialZoneDps);

			// VFX de la zone envoyé aux clients au moment de la pose (les zones n'existent que sur le serveur) :
			// environ 7 envois Reliable par traînée (une zone toutes les 0,375 s), fréquence faible, aucun retard visuel.
			if (UNiagaraSystem* ZoneSystem = Boss.ResolveFireZoneVFX())
			{
				Boss.Multicast_SpawnFireZoneVFX(ZoneSystem, TArray<FVector>{ Impact });
			}
		}
		break;
	}

	case EEMBBreathMode::Cataclysm:
		break; // dégâts appliqués une seule fois dans BeginFiring
	}

	++FireTickCount;

	if (Row.bAppliesBurn)
	{
		for (AActor* Victim : Landed)
		{
			Boss.ApplyBurn(Victim);
		}
	}
}

EBTNodeResult::Type UBTTask_EMBDragonBreath::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBDragonBreath::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBDragonBreath::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (AEMBDragonBoss* Boss = BossPtr.Get())
	{
		Boss->Multicast_StopBreathVFX();

		Boss->SetFacingLocked(false);
		if (bNeedsFlight && Boss->IsFlying())
		{
			Boss->SetFlying(false); // retour au sol à la fin du souffle aérien / du Cataclysm
		}
	}
	ActiveRow = nullptr;
}

FString UBTTask_EMBDragonBreath::GetStaticDescription() const
{
	return bUseCataclysmTag
		? TEXT("Souffle scripte : Cataclysm (charge de 4 s en vol, ligne de vue)")
		: TEXT("Souffle de l'attaque choisie (SelectedAttackTag) : cone, balayage ou aerien");
}
