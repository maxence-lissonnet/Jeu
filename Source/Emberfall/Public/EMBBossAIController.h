// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "EMBBossAIController.generated.h"

class UAISenseConfig_Sight;
class UBehaviorTree;
class UBlackboardData;

/**
 * Contrôleur IA générique des boss d'Emberfall (Colosse, Nyxariel, Vaelthyrax). Existe uniquement sur le serveur
 * (les AIController ne sont pas répliqués) : le Behavior Tree, la perception et le choix des cibles ne tournent donc
 * que côté serveur autoritaire.
 *
 * Fonctions : lancer le BT à la possession (sauf boss déjà vaincu), déclencher la rencontre à la première détection
 * d'un joueur, pousser les états critiques du boss dans le Blackboard, gérer la cible et le verrouillage du regard.
 *
 * Clés du Blackboard communes (nom, type) : TargetActor (Object/Actor), TargetDistance (Float), TargetAngle (Float,
 * signé, > 0 = à droite), CurrentPhase (Int), bIsFlying, bIsStunned, bIsTransitioning, bIsDead, bIsVulnerable,
 * bCataclysmDue (Bool : "attaque scriptée due", cf. AEMBBossBase::IsScriptedAttackDue), ThreatCount (Int),
 * SelectedAttackTag (Name), SelectedAttackKind (Int = EEMBDragonAttackKind), SelectedAttackDuration (Float).
 */
UCLASS()
class EMBERFALL_API AEMBBossAIController : public AAIController
{
	GENERATED_BODY()

public:
	AEMBBossAIController();

	// --- Clés du Blackboard (noms exacts à créer dans le Blackboard du boss) ---
	static const FName BBKey_TargetActor;
	static const FName BBKey_TargetDistance;
	static const FName BBKey_TargetAngle;
	static const FName BBKey_CurrentPhase;
	static const FName BBKey_IsFlying;
	static const FName BBKey_IsStunned;
	static const FName BBKey_IsTransitioning;
	static const FName BBKey_IsDead;
	static const FName BBKey_IsVulnerable;
	static const FName BBKey_CataclysmDue;
	static const FName BBKey_ThreatCount;
	static const FName BBKey_SelectedAttackTag;
	static const FName BBKey_SelectedAttackKind;
	static const FName BBKey_SelectedAttackDuration;

	/** Pousse immédiatement les états critiques du boss (phase, vol, étourdi, transition, mort) dans le Blackboard. */
	void SyncBlackboardState();

	/** Définit la cible de combat : écrit TargetActor et oriente le regard du boss (sauf si l'orientation est figée). */
	void SetCombatTarget(AActor* NewTarget);

	/** Fige (true) ou libère (false) le suivi de la cible du regard pendant l'engagement d'une attaque. */
	void SetFacingLocked(bool bLocked);

	AActor* GetCombatTarget() const { return CombatTarget.Get(); }

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UFUNCTION()
	void HandleTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);

	/** Arbre de comportement à exécuter. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|AI")
	TObjectPtr<UBehaviorTree> BehaviorTreeAsset;

	/** Blackboard associé. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|AI")
	TObjectPtr<UBlackboardData> BlackboardAsset;

	UPROPERTY(VisibleAnywhere, Category = "Emberfall|Boss|AI")
	TObjectPtr<UAISenseConfig_Sight> SightConfig;

private:
	void ApplyFocus();

	TWeakObjectPtr<AActor> CombatTarget;
	bool bFacingLocked = false;
};
