// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBCreatureFlee.h"

#include "EMBCreatureAIController.h"
#include "EMBCreatureBase.h"
#include "EMBEnvironmentSubsystem.h"

#include "AIController.h"
#include "AITypes.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"

namespace
{
	/** Nombre de directions candidates autour de la créature. */
	constexpr int32 FleeDirectionCount = 8;

	/** Points de chaleur échantillonnés par segment candidat (le dernier est l'arrivée projetée). */
	constexpr int32 FleeHeatSampleCount = 4;

	/** Tolérance (cm) de la projection d'un point candidat sur le navmesh. */
	const FVector NavProjectionExtent(300.f, 300.f, 500.f);
}

UBTTask_EMBCreatureFlee::UBTTask_EMBCreatureFlee()
{
	NodeName = TEXT("EMB Creature Flee");
	bCreateNodeInstance = true;
	bNotifyTick = true;
}

bool UBTTask_EMBCreatureFlee::ComputeFleeDestination(const AEMBCreatureBase& Creature, const AActor* Threat, FVector& OutDestination) const
{
	UWorld* World = Creature.GetWorld();
	UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(World);
	if (!World || !NavSystem)
	{
		return false;
	}

	const FVector Origin = Creature.GetActorLocation();
	const FVector ThreatLocation = Threat ? Threat->GetActorLocation() : Origin;

	// Une créature qui craint le feu n'accepte aucun point encore éclairé par une source de chaleur.
	const UEMBEnvironmentSubsystem* Environment = Creature.GetCreatureRow().bFleesFire ? World->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr;

	// Tri : chaleur maximale rencontrée sur le segment (croissante), puis éloignement de la menace (décroissant).
	// Un segment qui traverse une zone chaude (>= seuil) est donc écarté tant qu'un segment froid existe ; si la créature
	// est cernée par la chaleur, on retient le moins chaud plutôt que de renoncer à fuir.
	bool bFound = false;
	float BestHeat = 0.f;
	float BestDistance = 0.f;
	for (int32 Index = 0; Index < FleeDirectionCount; ++Index)
	{
		const float AngleRad = 2.f * PI * static_cast<float>(Index) / static_cast<float>(FleeDirectionCount);
		const FVector Direction(FMath::Cos(AngleRad), FMath::Sin(AngleRad), 0.f);

		FNavLocation Projected;
		if (!NavSystem->ProjectPointToNavigation(Origin + Direction * FleeDistance, Projected, NavProjectionExtent))
		{
			continue;
		}

		// Échantillonnage de la chaleur le long du segment (quart, moitié, trois quarts, arrivée projetée).
		float MaxHeat = 0.f;
		if (Environment)
		{
			for (int32 Sample = 1; Sample <= FleeHeatSampleCount; ++Sample)
			{
				const FVector Point = (Sample == FleeHeatSampleCount)
					? Projected.Location
					: Origin + Direction * (FleeDistance * static_cast<float>(Sample) / static_cast<float>(FleeHeatSampleCount));
				MaxHeat = FMath::Max(MaxHeat, Environment->GetHeatBonusAt(Point));
			}
		}

		const float Distance = static_cast<float>(FVector::Dist2D(Projected.Location, ThreatLocation));
		if (!bFound || MaxHeat < BestHeat || (FMath::IsNearlyEqual(MaxHeat, BestHeat) && Distance > BestDistance))
		{
			bFound = true;
			BestHeat = MaxHeat;
			BestDistance = Distance;
			OutDestination = Projected.Location;
		}
	}
	return bFound;
}

EBTNodeResult::Type UBTTask_EMBCreatureFlee::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AEMBCreatureAIController* AIController = Cast<AEMBCreatureAIController>(OwnerComp.GetAIOwner());
	AEMBCreatureBase* Creature = AIController ? Cast<AEMBCreatureBase>(AIController->GetPawn()) : nullptr;
	if (!Creature || !Creature->HasCreatureRow() || Creature->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	FVector Destination = FVector::ZeroVector;
	if (!ComputeFleeDestination(*Creature, AIController->GetCombatTarget(), Destination))
	{
		return EBTNodeResult::Failed;
	}

	Creature->CancelAttack(true);
	Creature->SetRunning(true);

	// Arguments : destination, rayon d'arrivée, arrêt au recouvrement, pathfinding, projection sur le navmesh, sans strafe.
	const EPathFollowingRequestResult::Type Request = AIController->MoveToLocation(Destination, AcceptanceRadius, true, true, true, false);
	if (Request == EPathFollowingRequestResult::Failed)
	{
		return EBTNodeResult::Failed;
	}
	if (Request == EPathFollowingRequestResult::AlreadyAtGoal)
	{
		return EBTNodeResult::Succeeded;
	}

	CreaturePtr = Creature;
	ElapsedTime = 0.f;
	return EBTNodeResult::InProgress;
}

void UBTTask_EMBCreatureFlee::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	const AEMBCreatureBase* Creature = CreaturePtr.Get();
	if (!AIController || !Creature || Creature->IsDead())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	ElapsedTime += DeltaSeconds;
	if (ElapsedTime >= MaxDuration)
	{
		AIController->StopMovement();
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
	else if (AIController->GetMoveStatus() == EPathFollowingStatus::Idle)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded); // arrivée (ou déplacement interrompu)
	}
}

EBTNodeResult::Type UBTTask_EMBCreatureFlee::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (AAIController* AIController = OwnerComp.GetAIOwner())
	{
		AIController->StopMovement();
	}
	CreaturePtr.Reset();
	return EBTNodeResult::Aborted;
}

FString UBTTask_EMBCreatureFlee::GetStaticDescription() const
{
	return FString::Printf(TEXT("Fuite a %.0f cm (loin de la menace, hors de la lumiere d'un feu), duree max %.1f s"), FleeDistance, MaxDuration);
}
