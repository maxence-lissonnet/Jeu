// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBSeraphReposition.h"

#include "EMBBossAIController.h"
#include "EMBSeraphBoss.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

UBTTask_EMBSeraphReposition::UBTTask_EMBSeraphReposition()
{
	NodeName = TEXT("EMB Seraph Reposition");

	// Une instance par arbre (état propre à l'exécution), tick activé, notification de fin pour le nettoyage.
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_EMBSeraphReposition::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBSeraphBoss* Boss = AIController ? Cast<AEMBSeraphBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	BossPtr = Boss;
	TargetPtr = Cast<AActor>(Blackboard->GetValueAsObject(AEMBBossAIController::BBKey_TargetActor));
	bCleanedUp = false;
	Elapsed = 0.f;
	Duration = FMath::FRandRange(FMath::Min(MinDuration, MaxDuration), FMath::Max(MinDuration, MaxDuration));
	OrbitSign = FMath::RandBool() ? 1.f : -1.f;

	// Phases 1-2 : Nyxariel vole. Après un étourdissement ou une transition de phase il est au sol : il décolle ici.
	if (Boss->ShouldFly() && !Boss->IsFlying())
	{
		Boss->SetFlying(true);
	}
	Boss->SetFacingLocked(false); // reste face à sa cible pendant l'orbite

	return EBTNodeResult::InProgress;
}

void UBTTask_EMBSeraphReposition::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBSeraphBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || Boss->IsStunned() || Boss->IsTransitioning())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	Elapsed += DeltaSeconds;

	const AActor* Target = TargetPtr.Get();
	if (Target && Boss->ShouldFly() && Boss->IsFlying())
	{
		// Rayon actuel de la cible vers le boss (à plat), tourné d'un petit angle à chaque image : orbite régulière,
		// recalée en continu sur la distance préférée même si la cible se déplace.
		const FVector TargetLocation = Target->GetActorLocation();
		FVector Radial = Boss->GetActorLocation() - TargetLocation;
		Radial.Z = 0.0;
		if (!Radial.Normalize())
		{
			Radial = -Boss->GetActorForwardVector().GetSafeNormal2D();
		}
		Radial = Radial.RotateAngleAxis(OrbitSign * EMBSeraphDefaults::OrbitDegreesPerSecond * DeltaSeconds, FVector::UpVector);

		FVector Destination = TargetLocation + Radial * Boss->GetPreferredCombatDistance();
		Destination.Z = TargetLocation.Z + Boss->GetHoverHeightAboveTarget();

		bool bBlocked = false;
		Boss->StepFlightToward(Destination, EMBSeraphDefaults::FlySpeed, DeltaSeconds, bBlocked);
	}

	if (Elapsed >= Duration)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

EBTNodeResult::Type UBTTask_EMBSeraphReposition::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBSeraphReposition::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBSeraphReposition::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (const AEMBSeraphBoss* Boss = BossPtr.Get())
	{
		if (Boss->IsFlying())
		{
			if (UCharacterMovementComponent* Move = Boss->GetCharacterMovement())
			{
				Move->StopMovementImmediately(); // pas de vitesse résiduelle répliquée une fois immobile en vol stationnaire
			}
		}
	}
}

FString UBTTask_EMBSeraphReposition::GetStaticDescription() const
{
	return FString::Printf(TEXT("Repositionnement de Nyxariel : %.1f a %.1f s (vol orbital en phases 1-2, attente au sol en phase 3)"),
		MinDuration, MaxDuration);
}
