// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBDragonDiveAttack.h"

#include "EMBDragonAIController.h"
#include "EMBDragonBoss.h"
#include "EMBGameplayTags.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"

namespace
{
	/** Durée maximale de la plongée (s) : garde-fou si le balayage ne rencontre jamais le sol ni la destination. */
	constexpr float MaxDiveDuration = 3.f;

	/** Vitesse de rotation du dragon vers le point marqué pendant l'étape Mark (degrés/s). */
	constexpr float MarkAlignSpeed = 180.f;

	/** Plafond du décalage de prédiction de la position de la cible (cm). */
	constexpr float DiveMaxPredictionOffset = 800.f;
}

UBTTask_EMBDragonDiveAttack::UBTTask_EMBDragonDiveAttack()
{
	NodeName = TEXT("EMB Dragon Dive");
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_EMBDragonDiveAttack::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBDragonBoss* Boss = AIController ? Cast<AEMBDragonBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	const FGameplayTag DiveTag = EMBTags::Attack_Dragon_Dive.GetTag();
	const FEMBDragonAttackRow* Row = Boss->FindAttackRow(DiveTag);
	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBDragonAIController::BBKey_TargetActor));
	if (!Row || !Target)
	{
		return EBTNodeResult::Failed;
	}

	// --- Initialisation (aucun effet de bord avant ce point) ---
	// Annule d'abord toute attaque programmée par le boss (impacts de mêlée, braises) encore en cours.
	Boss->CancelTimedAttack(true);
	BossPtr = Boss;
	TargetPtr = Target;
	ActiveRow = Row;
	bCleanedUp = false;
	bFlashSent = false;
	StageTime = 0.f;
	MarkClock = 0.f;
	DiveElapsed = 0.f;
	LaunchTime = 0.f;
	Stage = EDiveStage::Takeoff;

	const float TelegraphMultiplier = Boss->GetTelegraphMultiplier();
	TakeoffDuration = EMBDragonDefaults::DiveTakeoffDuration * TelegraphMultiplier;
	MarkDuration = EMBDragonDefaults::DiveMarkDuration * TelegraphMultiplier;
	ImpactRadius = Row->EffectRadius > 0.f ? Row->EffectRadius : EMBDragonDefaults::DiveImpactRadius;

	Boss->Multicast_Telegraph(DiveTag, TakeoffDuration + MarkDuration);
	if (UAnimMontage* Montage = Boss->ResolveMontage(Row->Montage))
	{
		Boss->PlayAttackMontage(Montage, Boss->GetAttackPlayRate());
	}

	Boss->SetFacingLocked(false);
	Boss->SetFlying(true);

	StartZ = static_cast<float>(Boss->GetActorLocation().Z);
	CruiseZ = StartZ + EMBDragonDefaults::DiveCruiseAltitude;

	return EBTNodeResult::InProgress;
}

void UBTTask_EMBDragonDiveAttack::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBDragonBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || !ActiveRow)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	switch (Stage)
	{
	case EDiveStage::Takeoff:
	{
		// Montée à vitesse constante : l'altitude de croisière est atteinte exactement à la fin du décollage.
		StageTime += DeltaSeconds;
		const FVector Location = Boss->GetActorLocation();
		const float TakeoffClimbSpeed = (CruiseZ - StartZ) / FMath::Max(TakeoffDuration, KINDA_SMALL_NUMBER);
		bool bBlocked = false;
		Boss->StepFlightToward(FVector(Location.X, Location.Y, CruiseZ), TakeoffClimbSpeed, DeltaSeconds, bBlocked);

		if (StageTime >= TakeoffDuration)
		{
			BeginMark(*Boss);
		}
		break;
	}

	case EDiveStage::Mark:
	{
		StageTime += DeltaSeconds;
		MarkClock += DeltaSeconds;

		const float YawToMarker = static_cast<float>((MarkerLocation - Boss->GetActorLocation()).Rotation().Yaw);
		Boss->RotateYawToward(YawToMarker, DeltaSeconds, MarkAlignSpeed);

		TickMarkFlash(*Boss);

		if (StageTime >= LaunchTime)
		{
			Stage = EDiveStage::Dive;
			DiveElapsed = 0.f;
		}
		break;
	}

	case EDiveStage::Dive:
	{
		MarkClock += DeltaSeconds;
		DiveElapsed += DeltaSeconds;
		TickMarkFlash(*Boss);

		bool bBlocked = false;
		const bool bArrived = Boss->StepFlightToward(DiveDestination, EMBDragonDefaults::DiveSpeed, DeltaSeconds, bBlocked);
		if (bArrived || bBlocked || DiveElapsed >= MaxDiveDuration)
		{
			DoImpact(*Boss);
		}
		break;
	}

	case EDiveStage::Vulnerable:
	{
		StageTime += DeltaSeconds;
		if (StageTime >= EMBDragonDefaults::DiveVulnerableDuration)
		{
			FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		}
		break;
	}
	}
}

void UBTTask_EMBDragonDiveAttack::BeginMark(AEMBDragonBoss& Boss)
{
	Stage = EDiveStage::Mark;
	StageTime = 0.f;
	MarkClock = 0.f;
	bFlashSent = false;

	// Position prédite : la cible est supposée continuer sur sa trajectoire pendant DivePredictionLead secondes
	// (décalage plafonné). Le joueur qui change de direction après l'apparition de l'ombre l'évite.
	const AActor* Target = TargetPtr.Get();
	FVector Predicted = Boss.GetActorLocation();
	if (Target)
	{
		Predicted = Target->GetActorLocation()
			+ (Target->GetVelocity() * EMBDragonDefaults::DivePredictionLead).GetClampedToMaxSize(DiveMaxPredictionOffset);
	}

	MarkerLocation = Boss.ProjectToGround(Predicted);
	DiveDestination = MarkerLocation + FVector(0.f, 0.f, Boss.GetSimpleCollisionHalfHeight());

	// Calage : le piqué est lancé "temps de trajet" avant la fin du marqueur pour que l'impact tombe à MarkDuration.
	const float TravelTime = static_cast<float>(FVector::Dist(Boss.GetActorLocation(), DiveDestination)) / EMBDragonDefaults::DiveSpeed;
	LaunchTime = FMath::Max(0.f, MarkDuration - TravelTime);

	Boss.Multicast_ShowGroundMarker(MarkerLocation, ImpactRadius, MarkDuration, false);
	Boss.SetFacingLocked(true); // le dragon ne suit plus la cible : seul le marqueur compte
}

void UBTTask_EMBDragonDiveAttack::TickMarkFlash(AEMBDragonBoss& Boss)
{
	// Flash de 0,35 s avant l'impact : dernier avertissement avant la plongée (fenêtre d'esquive).
	if (!bFlashSent && MarkClock >= MarkDuration - EMBDragonDefaults::DiveFlashDuration)
	{
		bFlashSent = true;
		Boss.Multicast_ShowGroundMarker(MarkerLocation, ImpactRadius, EMBDragonDefaults::DiveFlashDuration, true);
	}
}

void UBTTask_EMBDragonDiveAttack::DoImpact(AEMBDragonBoss& Boss)
{
	const FEMBDragonAttackRow& Row = *ActiveRow;

	Boss.SetFlying(false);

	// Dégâts autour du point MARQUÉ (et non de la position finale du dragon) : ce que le joueur a vu est ce qui frappe.
	Boss.ApplyAreaDamage(MarkerLocation, ImpactRadius, Boss.MakeHitData(Row));

	if (UNiagaraSystem* System = Boss.ResolveVFX(Row.VFX))
	{
		Boss.Multicast_SpawnVFXAtLocation(System, MarkerLocation, FRotator::ZeroRotator, FVector::OneVector);
	}

	Boss.SetFacingLocked(false);
	Boss.BeginVulnerableWindow(EMBDragonDefaults::DiveVulnerableDuration);

	Stage = EDiveStage::Vulnerable;
	StageTime = 0.f;
}

EBTNodeResult::Type UBTTask_EMBDragonDiveAttack::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBDragonDiveAttack::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBDragonDiveAttack::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (AEMBDragonBoss* Boss = BossPtr.Get())
	{
		Boss->SetFacingLocked(false);
		if (Boss->IsFlying())
		{
			Boss->SetFlying(false); // abandon en plein vol : le dragon retombe au sol
		}
	}
	ActiveRow = nullptr;
}

FString UBTTask_EMBDragonDiveAttack::GetStaticDescription() const
{
	return TEXT("Pique : decollage 2 s, ombre predite 1,5 s, plongee 4 500 cm/s, impact, vulnerable 2,5 s");
}
