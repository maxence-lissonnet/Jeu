// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "EMBCreatureTypes.h"
#include "BTTask_EMBCreatureFlee.generated.h"

class AEMBCreatureBase;

/**
 * Fuite d'une créature légère (branche "Fuite" de BT_Creature, voir EMBCreatureAIController.h) : goule qui fuit un
 * feu de camp ou une forge, wyverneau sous 30 % de PV.
 *
 * Choisit parmi 8 directions le point navigable à FleeDistance qui maximise l'éloignement de la menace (la cible, ou
 * la créature elle-même quand seul le feu la chasse) en écartant tout point encore dans la lumière d'une source de
 * chaleur (si la créature craint le feu). Déplacement par AAIController::MoveToLocation (pathfinding). La tâche
 * réussit à l'arrivée ou après MaxDuration ; elle est abandonnée (arrêt du déplacement) quand bShouldFlee retombe.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBCreatureFlee : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBCreatureFlee();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** Distance (cm) de chaque fuite. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Creature", meta = (ClampMin = "300.0"))
	float FleeDistance = EMBCreatureDefaults::FleeDistance;

	/** Durée maximale (s) d'une fuite avant réévaluation par le BT. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Creature", meta = (ClampMin = "0.5"))
	float MaxDuration = EMBCreatureDefaults::FleeMaxDuration;

	/** Rayon d'arrivée (cm). */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Creature", meta = (ClampMin = "10.0"))
	float AcceptanceRadius = 150.f;

private:
	bool ComputeFleeDestination(const AEMBCreatureBase& Creature, const AActor* Threat, FVector& OutDestination) const;

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBCreatureBase> CreaturePtr;
	float ElapsedTime = 0.f;
};
