// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBSeraphPlunge.h"

#include "EMBBossAIController.h"
#include "EMBBossTags.h"
#include "EMBSeraphBoss.h"

#include "AIController.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	/** Une étape de montée plus courte que ceci est ignorée (s). */
	constexpr float MinClimbDuration = 0.02f;

	/** Marge de vitesse du suivi de trajectoire : le point voulu est atteint dans l'image, sans dépassement. */
	constexpr float PathFollowSpeedMargin = 1.05f;

	/**
	 * Position sur un trajet à l'instant normalisé Alpha (0..1) : segment Start-End, plus un arc vertical (bond) et une
	 * hélice (spirale) dont l'enveloppe 4a(1-a) est nulle au départ et à l'arrivée. Aucune discontinuité, et l'arrivée
	 * tombe exactement sur End : la position est une fonction du TEMPS, la durée du trajet est donc exacte.
	 */
	FVector EvaluatePath(const FVector& Start, const FVector& End, const float Alpha, const float ArcHeight, const float HelixRadius, const float HelixTurns)
	{
		const float A = FMath::Clamp(Alpha, 0.f, 1.f);
		const float Envelope = 4.f * A * (1.f - A);

		FVector Position = Start + (End - Start) * A;
		Position.Z += ArcHeight * Envelope;

		if (HelixRadius > 0.f)
		{
			const FVector Axis = (End - Start).GetSafeNormal();
			FVector Right = FVector::CrossProduct(Axis, FVector::UpVector).GetSafeNormal();
			if (Right.IsNearlyZero())
			{
				Right = FVector::RightVector; // trajet vertical
			}
			const FVector Up = FVector::CrossProduct(Right, Axis).GetSafeNormal();
			const float Angle = A * HelixTurns * UE_TWO_PI;
			Position += (Right * FMath::Cos(Angle) + Up * FMath::Sin(Angle)) * (HelixRadius * Envelope);
		}
		return Position;
	}
}

UBTTask_EMBSeraphPlunge::UBTTask_EMBSeraphPlunge()
{
	NodeName = TEXT("EMB Seraph Plunge");

	// Une instance par arbre (état propre à l'exécution), tick activé, notification de fin pour le nettoyage.
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_EMBSeraphPlunge::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBSeraphBoss* Boss = AIController ? Cast<AEMBSeraphBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Blackboard->GetValueAsName(AEMBBossAIController::BBKey_SelectedAttackTag), false);
	const bool bIsSpiral = Tag == EMBBossTags::Attack_Seraph_SpiralDive.GetTag();
	const bool bIsDeadStar = Tag == EMBBossTags::Attack_Seraph_DeadStarFall.GetTag();
	const FEMBDragonAttackRow* Row = (bIsSpiral || bIsDeadStar) ? Boss->FindAttackRow(Tag) : nullptr;
	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBBossAIController::BBKey_TargetActor));
	if (!Row || !Target)
	{
		return EBTNodeResult::Failed;
	}

	// --- Initialisation (aucun effet de bord avant ce point) ---
	// Annule d'abord toute attaque programmée par le boss encore en cours.
	Boss->CancelTimedAttack(true);

	BossPtr = Boss;
	TargetPtr = Target;
	ActiveRow = Row;
	ActiveTag = Tag;
	Kind = bIsSpiral ? EPlungeKind::SpiralDive : EPlungeKind::DeadStarFall;
	Stage = EStage::Moving;
	bCleanedUp = false;
	Clock = 0.f;
	StageTime = 0.f;
	LegIndex = 0;
	LegCount = 0;
	bMarkerShown = false;
	bFlashShown = false;
	HitTemplate = Boss->MakeSeraphHitData(*Row);

	const float TelegraphMultiplier = Boss->GetTelegraphMultiplier();
	MarkDuration = Row->TelegraphTime * TelegraphMultiplier;
	ImpactRadius = Row->EffectRadius > 0.f ? Row->EffectRadius : EMBSeraphDefaults::DiveImpactRadius;

	Boss->Multicast_Telegraph(Tag, MarkDuration);
	if (UAnimMontage* Montage = Boss->ResolveMontage(Row->Montage))
	{
		Boss->PlayAttackMontage(Montage, Boss->GetAttackPlayRate());
	}

	// Le boss ne suit plus sa cible du regard : seul le point marqué compte (rotation pilotée par la tâche).
	Boss->SetFacingLocked(true);
	// Phases 1-2 : déjà en vol. Phase 3 : vol activé pour la durée de l'attaque seulement (Cleanup le coupe).
	Boss->SetFlying(true);

	if (Kind == EPlungeKind::SpiralDive)
	{
		BeginSpiral(*Boss);
	}
	else
	{
		BeginDeadStar(*Boss);
	}
	return EBTNodeResult::InProgress;
}

void UBTTask_EMBSeraphPlunge::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBSeraphBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || Boss->IsStunned() || Boss->IsTransitioning() || !ActiveRow)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	Clock += DeltaSeconds;

	switch (Stage)
	{
	case EStage::Moving:
		TickMarkers(*Boss);
		TickLeg(*Boss, DeltaSeconds);
		break;

	case EStage::Recover:
		StageTime += DeltaSeconds;
		if (StageTime >= EMBSeraphDefaults::PlungeRecoverTime)
		{
			FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		}
		break;

	case EStage::Vulnerable:
		StageTime += DeltaSeconds;
		if (StageTime >= EMBSeraphDefaults::DeadStarVulnerableDuration)
		{
			FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		}
		break;
	}
}

// ----------------------------------------------------------------------------------------------
// Préparation
// ----------------------------------------------------------------------------------------------

void UBTTask_EMBSeraphPlunge::BeginSpiral(AEMBSeraphBoss& Boss)
{
	// Point marqué = position PRÉDITE de la cible ; l'ombre apparaît tout de suite, le piqué est calé sur sa fin.
	ImpactPoint = Boss.PredictTargetGroundPoint(TargetPtr.Get(), EMBSeraphDefaults::DivePredictionLead);

	const FVector Start = Boss.GetActorLocation();
	const float CruiseZ = static_cast<float>(ImpactPoint.Z) + EMBSeraphDefaults::SpiralCruiseAltitude;
	SpiralDiveStart = FVector(Start.X, Start.Y, FMath::Max(static_cast<float>(Start.Z), CruiseZ)); // jamais de descente préalable
	const FVector DiveEnd = ImpactPoint + FVector(0.f, 0.f, Boss.GetSimpleCollisionHalfHeight());

	SpiralTravelTime = static_cast<float>(FVector::Dist(SpiralDiveStart, DiveEnd)) / EMBSeraphDefaults::SpiralDiveSpeed;

	// Montée : au plus vite SpiralClimbMaxSpeed ; plus lente si le trajet de plongée est court (l'impact reste calé).
	const float MinClimbTime = static_cast<float>(SpiralDiveStart.Z - Start.Z) / EMBSeraphDefaults::SpiralClimbMaxSpeed;
	SpiralClimbDuration = FMath::Max(MinClimbTime, MarkDuration - SpiralTravelTime);

	const float ImpactTime = SpiralClimbDuration + SpiralTravelTime;
	Boss.Multicast_ShowGroundMarker(ImpactPoint, ImpactRadius, ImpactTime, false);
	bMarkerShown = true;
	FlashClock = ImpactTime - EMBSeraphDefaults::DiveFlashDuration;

	LegCount = SpiralClimbDuration > MinClimbDuration ? 2 : 1;
	LegIndex = 0;
	StartLeg(Boss);
}

void UBTTask_EMBSeraphPlunge::BeginDeadStar(AEMBSeraphBoss& Boss)
{
	LegCount = (ActiveRow && ActiveRow->ImpactCount > 0) ? ActiveRow->ImpactCount : EMBSeraphDefaults::DeadStarPlungeCount;
	DeadStarInterval = (ActiveRow && ActiveRow->ActiveDuration > 0.f) ? ActiveRow->ActiveDuration : EMBSeraphDefaults::DeadStarInterval;
	LegIndex = 0;
	StartLeg(Boss);
}

void UBTTask_EMBSeraphPlunge::StartLeg(AEMBSeraphBoss& Boss)
{
	LegElapsed = 0.f;
	LegStart = Boss.GetActorLocation();
	LegArcHeight = 0.f;
	LegHelixRadius = 0.f;
	LegHelixTurns = 0.f;

	if (Kind == EPlungeKind::SpiralDive)
	{
		const bool bClimbLeg = LegCount == 2 && LegIndex == 0;
		if (bClimbLeg)
		{
			// Montée à l'aplomb de l'ombre.
			LegEnd = SpiralDiveStart;
			LegDuration = SpiralClimbDuration;
			bLegHasImpact = false;
		}
		else
		{
			// Plongée en hélice décroissante vers le point marqué, arrivée à l'instant exact de la fin de l'ombre.
			LegEnd = ImpactPoint + FVector(0.f, 0.f, Boss.GetSimpleCollisionHalfHeight());
			LegDuration = FMath::Max(SpiralTravelTime, KINDA_SMALL_NUMBER);
			LegHelixRadius = EMBSeraphDefaults::SpiralHelixRadius;
			LegHelixTurns = EMBSeraphDefaults::SpiralHelixTurns;
			bLegHasImpact = true;
		}
		return;
	}

	// Chute de l'étoile morte : bond en arc vers le point prédit au début du bond. Le 1er bond dure l'ombre (0,8 s),
	// les suivants l'intervalle entre impacts (0,9 s) : l'ombre apparaît alors 0,1 s après l'impact précédent.
	ImpactPoint = Boss.PredictTargetGroundPoint(TargetPtr.Get(), EMBSeraphDefaults::DeadStarPredictionLead);
	LegEnd = ImpactPoint + FVector(0.f, 0.f, Boss.GetSimpleCollisionHalfHeight());
	LegDuration = FMath::Max(LegIndex == 0 ? MarkDuration : DeadStarInterval, MarkDuration);
	LegArcHeight = EMBSeraphDefaults::DeadStarArcHeight;
	bLegHasImpact = true;

	MarkerShowClock = Clock + (LegDuration - MarkDuration);
	FlashClock = Clock + LegDuration - EMBSeraphDefaults::DiveFlashDuration;
	bMarkerShown = false;
	bFlashShown = false;
}

// ----------------------------------------------------------------------------------------------
// Déroulement
// ----------------------------------------------------------------------------------------------

void UBTTask_EMBSeraphPlunge::TickMarkers(AEMBSeraphBoss& Boss)
{
	if (!bMarkerShown && Clock >= MarkerShowClock)
	{
		bMarkerShown = true;
		Boss.Multicast_ShowGroundMarker(ImpactPoint, ImpactRadius, MarkDuration, false);
	}

	// Flash de 0,35 s avant l'impact : dernier avertissement (fenêtre d'esquive).
	if (bMarkerShown && !bFlashShown && Clock >= FlashClock)
	{
		bFlashShown = true;
		Boss.Multicast_ShowGroundMarker(ImpactPoint, ImpactRadius, EMBSeraphDefaults::DiveFlashDuration, true);
	}
}

void UBTTask_EMBSeraphPlunge::TickLeg(AEMBSeraphBoss& Boss, const float DeltaSeconds)
{
	LegElapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(LegElapsed / FMath::Max(LegDuration, KINDA_SMALL_NUMBER), 0.f, 1.f);

	// Suivi de trajectoire : on vise la position voulue à cet instant, avec la vitesse qui l'atteint dans l'image.
	// Le balayage de StepFlightToward arrête le boss contre le décor ; l'obstacle est ignoré sauf à l'arrivée.
	const FVector Desired = EvaluatePath(LegStart, LegEnd, Alpha, LegArcHeight, LegHelixRadius, LegHelixTurns);
	const float Distance = static_cast<float>(FVector::Dist(Boss.GetActorLocation(), Desired));
	const float FollowSpeed = FMath::Max(Distance / FMath::Max(DeltaSeconds, KINDA_SMALL_NUMBER) * PathFollowSpeedMargin, 1.f);
	bool bBlocked = false;
	Boss.StepFlightToward(Desired, FollowSpeed, DeltaSeconds, bBlocked);

	const float YawToMarker = static_cast<float>((ImpactPoint - Boss.GetActorLocation()).Rotation().Yaw);
	Boss.RotateYawToward(YawToMarker, DeltaSeconds, EMBSeraphDefaults::PlungeMarkAlignSpeed);

	if (Alpha >= 1.f)
	{
		FinishLeg(Boss);
	}
}

void UBTTask_EMBSeraphPlunge::FinishLeg(AEMBSeraphBoss& Boss)
{
	if (bLegHasImpact)
	{
		DoImpact(Boss);
	}

	++LegIndex;
	if (LegIndex < LegCount)
	{
		StartLeg(Boss);
		return;
	}

	if (Kind == EPlungeKind::SpiralDive)
	{
		Stage = EStage::Recover;
		StageTime = 0.f;
	}
	else
	{
		BeginVulnerable(Boss);
	}
}

void UBTTask_EMBSeraphPlunge::DoImpact(AEMBSeraphBoss& Boss)
{
	if (!ActiveRow)
	{
		return;
	}

	// Dégâts autour du point MARQUÉ (et non de la position finale du boss) : ce que le joueur a vu est ce qui frappe.
	Boss.ApplyAreaDamage(ImpactPoint, ImpactRadius, HitTemplate);

	if (UNiagaraSystem* System = Boss.ResolveVFX(ActiveRow->VFX))
	{
		Boss.Multicast_SpawnVFXAtLocation(System, ImpactPoint, FRotator::ZeroRotator, FVector::OneVector);
	}

	// Les ombres jumelles frappent le même point 0,3 s plus tard.
	FEMBSeraphEcho Echo;
	Echo.AttackTag = ActiveTag;
	Echo.EventIndex = LegIndex;
	Echo.AimPoint = ImpactPoint;
	Boss.BroadcastShadowEcho(Echo);
}

void UBTTask_EMBSeraphPlunge::BeginVulnerable(AEMBSeraphBoss& Boss)
{
	// Après le 3e plongeon : retour au sol (phase 3), orientation libérée, vulnérable 4 s.
	Boss.SetFacingLocked(false);
	Boss.SetFlying(false);
	Boss.BeginVulnerableWindow(EMBSeraphDefaults::DeadStarVulnerableDuration);

	Stage = EStage::Vulnerable;
	StageTime = 0.f;
}

// ----------------------------------------------------------------------------------------------
// Fin et abandon
// ----------------------------------------------------------------------------------------------

EBTNodeResult::Type UBTTask_EMBSeraphPlunge::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBSeraphPlunge::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBSeraphPlunge::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (AEMBSeraphBoss* Boss = BossPtr.Get())
	{
		Boss->SetFacingLocked(false);
		if (UCharacterMovementComponent* Move = Boss->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
		// Phase 3 : le vol n'existe que pendant l'attaque. Abandon en plein bond : Nyxariel retombe au sol.
		if (Kind == EPlungeKind::DeadStarFall && Boss->IsFlying() && !Boss->ShouldFly())
		{
			Boss->SetFlying(false);
		}
	}
	ActiveRow = nullptr;
	HitTemplate = FEMBHitData();
}

FString UBTTask_EMBSeraphPlunge::GetStaticDescription() const
{
	return TEXT("Plongeons de Nyxariel : pique en spirale (ombre 1,2 s) ou chute de l'etoile morte (3 bonds, vulnerable 4 s)");
}
