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
#include "EMBBossAIController.h"
#include "EMBDragonAIController.generated.h"

/**
 * Contrôleur IA de Vaelthyrax. Toute la logique (possession, Blackboard, perception, cible) vit dans
 * AEMBBossAIController ; cette classe fine conserve le nom historique et porte la documentation du Behavior Tree
 * du dragon (en-tête de ce fichier). Les constantes BBKey_* sont héritées de la base.
 */
UCLASS()
class EMBERFALL_API AEMBDragonAIController : public AEMBBossAIController
{
	GENERATED_BODY()
};
