// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTService_EMBCreatureContext.h"

#include "EMBCreatureAIController.h"
#include "EMBCreatureBase.h"
#include "EMBCreatureTypes.h"
#include "EMBEnvironmentSubsystem.h"

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Engine/World.h"

UBTService_EMBCreatureContext::UBTService_EMBCreatureContext()
{
	NodeName = TEXT("EMB Creature Context");

	// 0,25 s +/- 0,05 : 3 à 5 mises à jour par seconde, réparties pour éviter les pics synchronisés entre créatures.
	Interval = 0.25f;
	RandomDeviation = 0.05f;
	bNotifyTick = true;
}

void UBTService_EMBCreatureContext::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	AEMBCreatureAIController* Controller = Cast<AEMBCreatureAIController>(OwnerComp.GetAIOwner());
	AEMBCreatureBase* Creature = Controller ? Cast<AEMBCreatureBase>(Controller->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	const UWorld* World = Creature ? Creature->GetWorld() : nullptr;
	if (!Creature || !Blackboard || !World || !Creature->HasCreatureRow())
	{
		return;
	}

	Controller->SyncBlackboardState();
	if (Creature->IsDead())
	{
		return;
	}

	const FEMBCreatureRow& Row = Creature->GetCreatureRow();
	const FVector Location = Creature->GetActorLocation();
	const FVector Home = Creature->GetHomeLocation();
	const float HomeDistance = static_cast<float>(FVector::Dist2D(Location, Home));

	// --- Laisse : au-delà de LeashRadius la poursuite est abandonnée ; la créature rentre jusqu'à son point d'apparition ---
	bool bReturnHome = Controller->IsReturningHome();
	if (!bReturnHome && HomeDistance > Row.LeashRadius)
	{
		bReturnHome = true;
	}
	else if (bReturnHome && HomeDistance <= EMBCreatureDefaults::ReturnHomeReachedRadius)
	{
		bReturnHome = false;
	}
	Controller->SetReturningHome(bReturnHome);

	// --- Cible : ignorée pendant le retour (une créature qui rentre ne reprend pas la poursuite) ---
	AActor* Target = bReturnHome ? nullptr : Controller->FindBestPlayerTarget(Controller->GetCombatTarget());
	if (Target != Controller->GetCombatTarget())
	{
		Controller->SetCombatTarget(Target);
	}

	// --- Fuite : feu (lumière d'une source de chaleur >= 10 °C) ou PV sous le seuil, avec une menace en vue ---
	bool bShouldFlee = false;
	if (Row.bFleesFire)
	{
		const UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>();
		bShouldFlee = Environment && Environment->GetHeatBonusAt(Location) >= EMBCreatureDefaults::FleeHeatBonusThreshold;
	}
	if (!bShouldFlee && Target && Creature->IsBelowFleeThreshold())
	{
		bShouldFlee = true;
	}

	// --- Données dérivées de la cible ---
	bool bInAttackRange = false;
	float TargetDistance = 0.f;
	if (Target)
	{
		TargetDistance = static_cast<float>(FVector::Dist2D(Location, Target->GetActorLocation()));
		bInAttackRange = !bShouldFlee && Creature->HasAttackInRange(Target);
	}

	Blackboard->SetValueAsFloat(AEMBCreatureAIController::BBKey_TargetDistance, TargetDistance);
	Blackboard->SetValueAsVector(AEMBCreatureAIController::BBKey_HomeLocation, Home);
	Blackboard->SetValueAsBool(AEMBCreatureAIController::BBKey_ReturnHome, bReturnHome);
	Blackboard->SetValueAsBool(AEMBCreatureAIController::BBKey_ShouldFlee, bShouldFlee);
	Blackboard->SetValueAsBool(AEMBCreatureAIController::BBKey_TargetInAttackRange, bInAttackRange);

	Creature->SetRunning(Target != nullptr || bShouldFlee || bReturnHome);

	// --- Patrouille : nouveau point quand l'actuel est atteint, absent ou trop ancien ---
	if (!Target && !bShouldFlee && !bReturnHome)
	{
		const bool bHasPoint = Blackboard->IsVectorValueSet(AEMBCreatureAIController::BBKey_PatrolLocation);
		const bool bReached = bHasPoint && FVector::Dist2D(Location, Blackboard->GetValueAsVector(AEMBCreatureAIController::BBKey_PatrolLocation)) <= PatrolReachedRadius;
		const bool bStale = World->GetTimeSeconds() - Controller->GetLastPatrolPickTime() > static_cast<double>(PatrolRepickInterval);
		if (!bHasPoint || bReached || bStale)
		{
			Controller->PickPatrolLocation();
		}
	}
}

FString UBTService_EMBCreatureContext::GetStaticDescription() const
{
	return FString::Printf(TEXT("Cible (vue x meteo, ouie), laisse, fuite, patrouille (toutes les %.2f s +/- %.2f)"), Interval, RandomDeviation);
}
