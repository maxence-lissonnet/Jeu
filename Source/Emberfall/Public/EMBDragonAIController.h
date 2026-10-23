// Copyright ADWEB Conseil. Tous droits réservés.

/*
 * ===============================================================================================
 * STRUCTURE DU BEHAVIOR TREE ATTENDU : BT_Vaelthyrax (Blackboard : BB_Vaelthyrax)
 * ===============================================================================================
 * Le Behavior Tree est un asset à construire dans l'éditeur ; ce schéma fixe l'ordre des branches
 * (priorité décroissante de haut en bas) et les décorateurs/services à poser. Les clés du Blackboard
 * sont les constantes BBKey_* de cette classe (noms et types exacts ci-dessous).
 *
 * Root
 * └─ Selector "Racine"                       [Service : BTService_EMBUpdateCombatContext (0,2 s ± 0,05)]
 *    │
 *    ├─ 1. Sequence "Mort"                   [Décorateur Blackboard : bIsDead == true ; Observer Aborts : Both]
 *    │     └─ Wait (durée très longue)       (le BrainComponent est de toute façon arrêté par le boss à la mort)
 *    │
 *    ├─ 2. Sequence "Transition de phase"    [Décorateur Blackboard : bIsTransitioning == true ; Observer Aborts : Both]
 *    │     └─ Wait 0,25 s                    (rugissement joué par le boss ; invulnérable ; aucune attaque)
 *    │
 *    ├─ 3. Sequence "Étourdi"                [Décorateur Blackboard : bIsStunned == true ; Observer Aborts : Both]
 *    │     └─ Wait 0,25 s                    (4 s d'étourdissement ; riposte critique x3 côté joueur)
 *    │
 *    ├─ 4. Sequence "Cataclysm scripté"      [Décorateur Blackboard : bCataclysmDue == true ; Observer Aborts : Lower Priority
 *    │     │                                  + BTDecorator_EMBDragonPhase (Min 3, Max 3)]
 *    │     └─ BTTask_EMBDragonBreath         (bUseCataclysmTag = true : 4 s de charge en vol, 900 dégâts, ligne de vue)
 *    │
 *    ├─ 5. Sequence "Combat"                 [Décorateur Blackboard : TargetActor is Set ; Observer Aborts : Self]
 *    │     ├─ BTTask_EMBSelectAttack         (échoue si aucune attaque valide -> la branche 6 prend le relais)
 *    │     ├─ Selector "Exécution par type"  (un décorateur Blackboard par enfant, sur la clé entière SelectedAttackKind)
 *    │     │    ├─ [SelectedAttackKind == 1 (Breath) ou == 5 (Scripted)]  BTTask_EMBDragonBreath (bUseCataclysmTag = false)
 *    │     │    ├─ [SelectedAttackKind == 2 (Dive)]                       BTTask_EMBDragonDiveAttack
 *    │     │    └─ [SelectedAttackKind == 0 (Melee), 3 (Area) ou 4 (Grab)]
 *    │     │         Wait Blackboard Time (clé : SelectedAttackDuration)  (impacts programmés par le boss)
 *    │     │         Valeurs de SelectedAttackKind = EEMBDragonAttackKind : Melee 0, Breath 1, Dive 2, Area 3, Grab 4, Scripted 5.
 *    │     │         Pour les types Melee / Area / Grab, dupliquer le noeud avec trois décorateurs, ou utiliser
 *    │     │         un décorateur composite "Or" (Melee, Area, Grab).
 *    │     └─ Wait 0,3 à 0,6 s (aléatoire)   (récupération entre deux attaques : fenêtre de punition pour le joueur)
 *    │
 *    └─ 6. Sequence "Repositionnement"       (aucun décorateur : branche de repli)
 *          ├─ MoveTo (TargetActor, rayon d'acceptation 700 cm)
 *          └─ Wait 0,3 s                     (évite de boucler à chaque frame quand tout est en recharge)
 *
 * Décorateurs de phase (BTDecorator_EMBDragonPhase, clé CurrentPhase, Observer Aborts : Both) : à poser sur les
 * branches ou sous-branches réservées à une phase (vol fréquent en P2, etc.). Le filtrage des attaques par phase
 * est déjà fait par SelectAttack à partir de la DataTable (AllowedPhases).
 *
 * Clés du Blackboard à déclarer (type) :
 *   TargetActor (Object, base Actor)     TargetDistance (Float)        TargetAngle (Float, signé, > 0 = à droite)
 *   CurrentPhase (Int)                   bIsFlying (Bool)              bIsStunned (Bool)
 *   bIsTransitioning (Bool)              bIsDead (Bool)                bIsVulnerable (Bool)
 *   bCataclysmDue (Bool)                 ThreatCount (Int)             SelectedAttackTag (Name)
 *   SelectedAttackKind (Int)             SelectedAttackDuration (Float)
 * ===============================================================================================
 */

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "EMBDragonAIController.generated.h"

class UAISenseConfig_Sight;
class UBehaviorTree;
class UBlackboardData;

/**
 * Contrôleur IA de Vaelthyrax. Existe uniquement sur le serveur (les AIController ne sont pas répliqués) :
 * le Behavior Tree, la perception et le choix des cibles ne tournent donc que côté serveur autoritaire.
 */
UCLASS()
class EMBERFALL_API AEMBDragonAIController : public AAIController
{
	GENERATED_BODY()

public:
	AEMBDragonAIController();

	// --- Clés du Blackboard (noms exacts à créer dans BB_Vaelthyrax) ---
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

	/** Définit la cible de combat : écrit TargetActor et oriente le regard du dragon (sauf si l'orientation est figée). */
	void SetCombatTarget(AActor* NewTarget);

	/** Fige (true) ou libère (false) le suivi de la cible du regard pendant l'engagement d'une attaque. */
	void SetFacingLocked(bool bLocked);

	AActor* GetCombatTarget() const { return CombatTarget.Get(); }

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	UFUNCTION()
	void HandleTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);

	/** Arbre de comportement à exécuter (BT_Vaelthyrax). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|AI")
	TObjectPtr<UBehaviorTree> BehaviorTreeAsset;

	/** Blackboard associé (BB_Vaelthyrax). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|AI")
	TObjectPtr<UBlackboardData> BlackboardAsset;

	UPROPERTY(VisibleAnywhere, Category = "Emberfall|Dragon|AI")
	TObjectPtr<UAISenseConfig_Sight> SightConfig;

private:
	void ApplyFocus();

	TWeakObjectPtr<AActor> CombatTarget;
	bool bFacingLocked = false;
};
