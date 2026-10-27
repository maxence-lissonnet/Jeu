// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBCreatureAttack.h"

#include "EMBCreatureAIController.h"
#include "EMBCreatureBase.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"

UBTTask_EMBCreatureAttack::UBTTask_EMBCreatureAttack()
{
	NodeName = TEXT("EMB Creature Attack");
	// Une instance par arbre : la créature et le temps restant sont propres au noeud.
	bCreateNodeInstance = true;
	bNotifyTick = true;
}

EBTNodeResult::Type UBTTask_EMBCreatureAttack::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBCreatureBase* Creature = AIController ? Cast<AEMBCreatureBase>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Creature || !Blackboard || !Creature->HasAuthority() || Creature->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBCreatureAIController::BBKey_TargetActor));
	if (!Target)
	{
		return EBTNodeResult::Failed;
	}

	// La sélection utilise la position ACTUELLE de la cible (et non la distance du Blackboard, rafraîchie à 4 Hz).
	const float Duration = Creature->StartAttackOn(Target);
	if (Duration <= 0.f)
	{
		return EBTNodeResult::Failed; // rien d'utilisable (recharges, portée) : le BT passe à la poursuite
	}

	CreaturePtr = Creature;
	RemainingTime = Duration;
	return EBTNodeResult::InProgress;
}

void UBTTask_EMBCreatureAttack::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	const AEMBCreatureBase* Creature = CreaturePtr.Get();
	if (!Creature || Creature->IsDead())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	RemainingTime -= DeltaSeconds;
	if (RemainingTime <= 0.f)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

EBTNodeResult::Type UBTTask_EMBCreatureAttack::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (AEMBCreatureBase* Creature = CreaturePtr.Get())
	{
		Creature->CancelAttack(true);
	}
	CreaturePtr.Reset();
	return EBTNodeResult::Aborted;
}

FString UBTTask_EMBCreatureAttack::GetStaticDescription() const
{
	return TEXT("Tirage pondere sur la table d'attaques de la creature, execution, attente de la fin de l'attaque");
}
