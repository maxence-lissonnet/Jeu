// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_EMBCreatureAttack.generated.h"

class AEMBCreatureBase;

/**
 * Attaque d'une créature légère (branche "Attaque" de BT_Creature, voir EMBCreatureAIController.h).
 *
 * Lit la cible dans TargetActor, demande à la créature de choisir une attaque (tirage pondéré anti-répétition sur sa
 * table : portée, angle, recharges) et de la lancer (AEMBCreatureBase::StartAttackOn). Les impacts sont programmés par
 * des timers de la créature ; la tâche est latente et se termine à la fin de l'attaque (durée rendue par la créature,
 * impacts + récupération). Échoue immédiatement si aucune attaque n'est utilisable : le BT bascule sur la poursuite.
 *
 * Abandon (étourdissement, fuite, mort, changement de branche) : l'attaque est annulée (impacts, orientation, montage).
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBCreatureAttack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBCreatureAttack();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual FString GetStaticDescription() const override;

private:
	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBCreatureBase> CreaturePtr;
	float RemainingTime = 0.f;
};
