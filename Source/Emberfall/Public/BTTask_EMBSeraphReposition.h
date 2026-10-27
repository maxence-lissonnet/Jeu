// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_EMBSeraphReposition.generated.h"

class AEMBSeraphBoss;

/**
 * Repositionnement de Nyxariel entre deux attaques : tâche latente de durée aléatoire (MinDuration à MaxDuration).
 *
 *  - Phases 1-2 (AEMBSeraphBoss::ShouldFly) : décolle si besoin (SetFlying(true)), puis tourne autour de sa cible à
 *    PreferredCombatDistance (~1 100 cm, milieu des plages de l'Estoc et de la Lance) et à HoverHeightAboveTarget
 *    au-dessus d'elle, dans un sens tiré au hasard. Vol scripté à FlySpeed (AEMBBossBase::StepFlightToward) : le
 *    combat reste mobile et lisible, jamais statique.
 *  - Phase 3 : Nyxariel est au sol, la tâche n'est qu'une attente de la même durée (le déplacement au sol est assuré
 *    par le MoveTo du Behavior Tree, branche de repli).
 *
 * Utilisée à deux endroits de l'arbre : fin de la séquence de combat (récupération 0,4 à 0,7 s, fenêtre de punition du
 * joueur) et branche de repli quand aucune attaque n'est valide (0,5 à 0,9 s). Aucun timer : bNotifyTick.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBSeraphReposition : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBSeraphReposition();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** Durée minimale du repositionnement (s). */
	UPROPERTY(EditAnywhere, Category = "Repositionnement", meta = (ClampMin = "0.05"))
	float MinDuration = 0.5f;

	/** Durée maximale du repositionnement (s). */
	UPROPERTY(EditAnywhere, Category = "Repositionnement", meta = (ClampMin = "0.05"))
	float MaxDuration = 0.9f;

private:
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBSeraphBoss> BossPtr;
	TWeakObjectPtr<AActor> TargetPtr;

	float Elapsed = 0.f;
	float Duration = 0.f;
	float OrbitSign = 1.f;
	bool bCleanedUp = true;
};
