// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTService_EMBUpdateCombatContext.h"

#include "EMBDragonAIController.h"
#include "EMBDragonBoss.h"
#include "EMBDragonTypes.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"

UBTService_EMBUpdateCombatContext::UBTService_EMBUpdateCombatContext()
{
	NodeName = TEXT("EMB Update Combat Context");

	// 0,2 s +/- 0,05 : 3 à 6 mises à jour par seconde, réparties pour éviter les pics synchronisés.
	Interval = 0.2f;
	RandomDeviation = 0.05f;
	bNotifyTick = true;
}

void UBTService_EMBUpdateCombatContext::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	AEMBDragonAIController* Controller = Cast<AEMBDragonAIController>(OwnerComp.GetAIOwner());
	AEMBDragonBoss* Boss = Controller ? Cast<AEMBDragonBoss>(Controller->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard)
	{
		return;
	}

	// États du boss (phase, vol, Cataclysm dû...). Le Cataclysm étant temporel, il doit être réévalué ici.
	Controller->SyncBlackboardState();

	// --- Choix de la cible ---
	TArray<APawn*> Players;
	AEMBDragonBoss::CollectAlivePlayerPawns(Boss->GetWorld(), Players);

	AActor* CurrentTarget = Controller->GetCombatTarget();
	AActor* BestTarget = nullptr;
	float BestScore = -1.f;

	for (APawn* Player : Players)
	{
		float Distance = 0.f;
		float Angle = 0.f;
		Boss->GetDistanceAndAngleTo(Player, Distance, Angle);
		if (Distance > MaxTargetDistance)
		{
			continue;
		}

		// Score = proximité (1 au contact, 0 à la portée maximale) + menace récente normalisée.
		float Score = (1.f - FMath::Clamp(Distance / MaxTargetDistance, 0.f, 1.f)) + Boss->GetThreatScore(Player);
		if (Player == CurrentTarget)
		{
			Score *= CurrentTargetBonus;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			BestTarget = Player;
		}
	}

	if (BestTarget)
	{
		// Début de rencontre dès qu'une cible est choisie (idempotent). La mise à l'échelle coop des PV est alors
		// figée : un joueur qui arrive plus tard ne change plus les PV du boss (arbitrage v1.1).
		Boss->BeginEncounter();
	}

	if (BestTarget != CurrentTarget)
	{
		Controller->SetCombatTarget(BestTarget);
	}

	// --- Données dérivées de la cible ---
	if (BestTarget)
	{
		float Distance = 0.f;
		float Angle = 0.f;
		Boss->GetDistanceAndAngleTo(BestTarget, Distance, Angle);
		Blackboard->SetValueAsFloat(AEMBDragonAIController::BBKey_TargetDistance, Distance);
		Blackboard->SetValueAsFloat(AEMBDragonAIController::BBKey_TargetAngle, Angle);
	}
	else
	{
		Blackboard->SetValueAsFloat(AEMBDragonAIController::BBKey_TargetDistance, 0.f);
		Blackboard->SetValueAsFloat(AEMBDragonAIController::BBKey_TargetAngle, 0.f);
	}

	Blackboard->SetValueAsInt(AEMBDragonAIController::BBKey_ThreatCount, Boss->CountPlayersWithin(EMBDragonDefaults::ThreatCountRadius));
}

FString UBTService_EMBUpdateCombatContext::GetStaticDescription() const
{
	return FString::Printf(TEXT("Cible, distance, angle, phase, vol, menace (toutes les %.2f s +/- %.2f)"), Interval, RandomDeviation);
}
