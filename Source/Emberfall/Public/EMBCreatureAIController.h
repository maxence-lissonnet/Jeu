// Copyright ADWEB Conseil. Tous droits réservés.

/*
 * ===============================================================================================
 * STRUCTURE DU BEHAVIOR TREE ATTENDU : BT_Creature (Blackboard : BB_Creature)
 * ===============================================================================================
 * Contrôleur : AEMBCreatureAIController (ou une classe fille Blueprint qui y assigne BT_Creature et BB_Creature).
 * Un seul arbre sert la goule et le wyverneau : tout ce qui change (vitesses, portées, fuite, attaques) vient de la
 * ligne de DT_Creatures. Priorité décroissante de haut en bas.
 *
 * Clés du Blackboard (nom exact, type) :
 *   TargetActor (Object/Actor), TargetDistance (Float), HomeLocation (Vector), PatrolLocation (Vector),
 *   bIsDead (Bool), bIsStaggered (Bool), bShouldFlee (Bool), bReturnHome (Bool), bTargetInAttackRange (Bool).
 *
 * Root
 * └─ Selector "Racine"                       [Service : BTService_EMBCreatureContext (0,25 s ± 0,05)]
 *    ├─ 1. Sequence "Mort"                   [Deco BB bIsDead == true, aborts Both]       -> Wait 10 s
 *    ├─ 2. Sequence "Étourdi"                [Deco BB bIsStaggered == true, aborts Both]  -> Wait 0,25 s
 *    ├─ 3. Sequence "Fuite"                  [Deco BB bShouldFlee == true, aborts Both]
 *    │      └─ BTTask_EMBCreatureFlee         (feu proche, ou PV sous FleeHealthFraction : s'éloigne en courant)
 *    ├─ 4. Sequence "Retour"                 [Deco BB bReturnHome == true, aborts Both]   (laisse dépassée)
 *    │     ├─ MoveTo (clé HomeLocation, rayon 200)
 *    │     └─ Wait 0,5 s                       (le service remet bReturnHome à false à l'arrivée)
 *    ├─ 5. Sequence "Combat"                 [Deco BB TargetActor is Set, aborts Both]
 *    │     ├─ Selector "Attaquer ou poursuivre"
 *    │     │    ├─ Sequence "Attaque"        [Deco BB bTargetInAttackRange == true, aborts Lower Priority]
 *    │     │    │     ├─ BTTask_EMBCreatureAttack   (tirage pondéré + exécution ; attend la durée de l'attaque)
 *    │     │    │     └─ Wait 0,3 à 0,6 s (aléatoire)   (récupération : fenêtre de punition pour le joueur)
 *    │     │    └─ Sequence "Poursuite"
 *    │     │          ├─ MoveTo (clé TargetActor, rayon 150)
 *    │     │          └─ Wait 0,25 s
 *    └─ 6. Sequence "Patrouille"             (aucune cible)
 *          ├─ MoveTo (clé PatrolLocation, rayon 150)
 *          └─ Wait 2 à 5 s (aléatoire)
 *
 * Perception, retour et choix de la cible : tout est dans BTService_EMBCreatureContext et dans ce contrôleur
 *  - vue (AISense_Sight, rayon de la ligne x GetVisibilityMultiplierAt) et ouïe (AISense_Hearing, bruits signalés,
 *    plus les joueurs en mouvement à moins de HearingRadius) ; le dernier agresseur reste une cible 6 s ;
 *  - laisse : au-delà de LeashRadius du point d'apparition, la créature abandonne et rentre (bReturnHome) ;
 *  - fuite : feu (GetHeatBonusAt >= 10 °C à l'endroit de la créature, si bFleesFire) ou PV <= FleeHealthFraction ;
 *    une cible qui se tient dans la lumière d'un feu n'est pas poursuivie par une créature qui fuit le feu.
 * Le Behavior Tree n'est lancé que sur le serveur (les AIController n'existent pas chez les clients).
 * ===============================================================================================
 */

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "EMBCreatureAIController.generated.h"

class UAISenseConfig_Hearing;
class UAISenseConfig_Sight;
class UBehaviorTree;
class UBlackboardData;

/**
 * Contrôleur IA des créatures légères (AEMBCreatureBase). Lance le BT à la possession, configure la perception à partir
 * de la ligne de données, gère la cible de combat, le regard, la laisse et la patrouille.
 */
UCLASS()
class EMBERFALL_API AEMBCreatureAIController : public AAIController
{
	GENERATED_BODY()

public:
	AEMBCreatureAIController();

	// --- Clés du Blackboard (noms exacts à créer dans BB_Creature) ---
	static const FName BBKey_TargetActor;
	static const FName BBKey_TargetDistance;
	static const FName BBKey_HomeLocation;
	static const FName BBKey_PatrolLocation;
	static const FName BBKey_IsDead;
	static const FName BBKey_IsStaggered;
	static const FName BBKey_ShouldFlee;
	static const FName BBKey_ReturnHome;
	static const FName BBKey_TargetInAttackRange;

	/** Pousse immédiatement les états critiques (mort, étourdissement) dans le Blackboard. */
	void SyncBlackboardState();

	/** Définit la cible de combat : écrit TargetActor et oriente le regard (sauf si l'orientation est figée). */
	void SetCombatTarget(AActor* NewTarget);
	AActor* GetCombatTarget() const { return CombatTarget.Get(); }

	/** Fige (true) ou libère (false) le suivi de la cible du regard pendant l'engagement d'une attaque. */
	void SetFacingLocked(bool bLocked);

	/**
	 * Meilleur joueur détecté : vue (portée x visibilité météo, rayon de perte élargi pour la cible actuelle), ouïe,
	 * ou dernier agresseur. Nullptr si personne. La cible actuelle est favorisée (évite les changements de cible).
	 */
	AActor* FindBestPlayerTarget(const AActor* CurrentTarget);

	/** État "retour au point d'apparition" (laisse dépassée), maintenu par le service. */
	bool IsReturningHome() const { return bReturningHome; }
	void SetReturningHome(bool bNewReturning) { bReturningHome = bNewReturning; }

	/** Choisit un point de patrouille atteignable dans PatrolRadius autour du point d'apparition et l'écrit dans le Blackboard. */
	bool PickPatrolLocation();
	double GetLastPatrolPickTime() const { return LastPatrolPickTime; }

protected:
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	/** Arbre de comportement à exécuter. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|AI")
	TObjectPtr<UBehaviorTree> BehaviorTreeAsset;

	/** Blackboard associé. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|AI")
	TObjectPtr<UBlackboardData> BlackboardAsset;

	UPROPERTY(VisibleAnywhere, Category = "Emberfall|Creature|AI")
	TObjectPtr<UAISenseConfig_Sight> SightConfig;

	UPROPERTY(VisibleAnywhere, Category = "Emberfall|Creature|AI")
	TObjectPtr<UAISenseConfig_Hearing> HearingConfig;

private:
	void ApplyFocus();
	void ConfigurePerceptionFromRow();

	TWeakObjectPtr<AActor> CombatTarget;
	bool bFacingLocked = false;
	bool bReturningHome = false;
	double LastPatrolPickTime = -1000.0;
};
