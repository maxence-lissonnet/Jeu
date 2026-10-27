// Copyright ADWEB Conseil. Tous droits réservés.

/*
 * ===============================================================================================
 * STRUCTURE DU BEHAVIOR TREE ATTENDU : BT_Seraph (Blackboard : BB_Seraph, mêmes clés que le Colosse et le dragon)
 * ===============================================================================================
 * Contrôleur : AEMBBossAIController (ou une classe fille). Priorité décroissante de haut en bas.
 *
 * Root
 * └─ Selector "Racine"                       [Service : BTService_EMBUpdateCombatContext (0,2 s ± 0,05)]
 *    ├─ 1. Sequence "Mort"                   [Deco BB bIsDead == true, aborts Both]          -> Wait long
 *    ├─ 2. Sequence "Transition de phase"    [Deco BB bIsTransitioning == true, aborts Both] -> Wait 0,25 s
 *    │      (rugissement 3 s, invulnérable ; la base pose Nyxariel au sol pendant la transition)
 *    ├─ 3. Sequence "Étourdi"                [Deco BB bIsStunned == true, aborts Both]       -> Wait 0,25 s
 *    │      (posture brisée 3 s, ou cloche 2 s ; BeginStun() le fait tomber au sol)
 *    ├─ 4. Sequence "Combat"                 [Deco BB TargetActor is Set, aborts Self]
 *    │     ├─ BTTask_EMBSelectAttack         (échoue si rien de valide : en phases 1-2 aucune attaque n'est permise
 *    │     │                                  tant que Nyxariel n'est pas en vol, voir IsAttackAllowed)
 *    │     ├─ Selector "Exécution par type"  (un décorateur BB par enfant, sur la clé entière SelectedAttackKind)
 *    │     │    ├─ [SelectedAttackKind == 9 (Dash)]    BTTask_EMBSeraphDash      (Estoc fulgurant, Feinte)
 *    │     │    ├─ [SelectedAttackKind == 10 (Plunge)] BTTask_EMBSeraphPlunge    (Piqué en spirale, Chute de l'étoile morte)
 *    │     │    └─ [SelectedAttackKind == 0 (Melee) ou 3 (Area)]
 *    │     │         Wait Blackboard Time (clé SelectedAttackDuration)  (impacts programmés par le boss :
 *    │     │         Ciseaux d'ailes, Pluie de plumes, Lance jetée, Halo noir, Ombres jumelles)
 *    │     │         Valeurs : Melee 0, Breath 1, Dive 2, Area 3, Grab 4, Scripted 5, Charge 6, Channel 7, Rings 8,
 *    │     │         Dash 9, Plunge 10.
 *    │     │         (dupliquer le noeud avec un décorateur par valeur, ou un décorateur composite "Or")
 *    │     └─ BTTask_EMBSeraphReposition     (récupération 0,4 à 0,7 s : vol orbital en phases 1-2 ; simple attente en
 *    │                                        phase 3 où Nyxariel est au sol). C'est la fenêtre de punition du joueur.
 *    └─ 5. Selector "Repositionnement" (repli quand aucune attaque n'est valide)
 *          ├─ [BTDecorator_EMBDragonPhase Min 1 / Max 2, aborts Self]
 *          │     BTTask_EMBSeraphReposition (0,5 à 0,9 s) : décollage si besoin, puis orbite à ~1 100 cm de la cible
 *          └─ [BTDecorator_EMBDragonPhase Min 3 / Max 3, aborts Self]
 *                Sequence : MoveTo(TargetActor, rayon 700) puis Wait 0,2 s   (phase 3 : au sol, vitesse x1,2)
 *
 * Remarques :
 *  - Aucun décorateur de phase n'est nécessaire dans la branche Combat : la table d'attaques filtre les phases
 *    (AllowedPhases) et AEMBSeraphBoss::IsAttackAllowed filtre l'état de vol.
 *  - Feinte : la levée dure 1,0 s, puis un délai aléatoire de 0,3 à 0,9 s (FRandomStream serveur du boss), puis un éclat
 *    de la pointe (Multicast_TipFlash) 0,15 s avant le dash. L'éclat est le signal d'esquive.
 *  - Ombres jumelles : les clones (AEMBSeraphShadow) rejouent chaque événement dommageable du boss 0,3 s plus tard,
 *    à 30 % des dégâts (BroadcastShadowEcho). Aucune tâche BT dédiée : l'invocation est un impact programmé.
 *  - Phase 3 (25 % des PV) : Nyxariel est au sol définitivement (aucun vol de repositionnement). La Chute de l'étoile
 *    morte reste possible : ses trois plongeons sont des bonds scriptés (vol limité à la durée de l'attaque, retour au
 *    sol garanti en fin ou en abandon).
 * ===============================================================================================
 */

#pragma once

#include "CoreMinimal.h"
#include "EMBBossBase.h"
#include "EMBSeraphBoss.generated.h"

class AEMBProjectile;
class AEMBSeraphShadow;

/**
 * Constantes de Nyxariel, l'Aile Flétrie (référentiel §11.3, GDD §9.2). Les valeurs marquées (*) ne figurent pas dans
 * le référentiel : ce sont des choix d'implémentation, à équilibrer en playtest.
 */
namespace EMBSeraphDefaults
{
	// --- Stats (§11.3) ---
	constexpr float MaxHealthSolo = 24000.f;
	constexpr float MaxPoise = 700.f;
	constexpr float PoiseRegenRate = 50.f;
	constexpr float PoiseRegenDelay = 2.5f;
	constexpr float StunDuration = 3.f;
	constexpr float FlySpeed = 1800.f;

	// --- Phases ---
	constexpr float Phase2HealthThreshold = 0.60f;
	constexpr float Phase3HealthThreshold = 0.25f;
	constexpr float PhaseTransitionDuration = 3.f;
	constexpr float Phase3SpeedMultiplier = 1.2f;
	constexpr float GroundWalkSpeed = 800.f;              // (*) vitesse de marche de base ; x1,2 en phase 3
	constexpr float RotationRateYaw = 240.f;              // (*) deg/s : combat rapide, pivote bien plus vite que le Colosse
	constexpr float ArenaRadius = 4000.f;                 // rayon de l'arène (propriété ArenaRadius de la base)

	// --- Résistances (fraction retranchée ; négatif = faiblesse) ---
	constexpr float ResistPhysical = 0.f;
	constexpr float ResistFire = -0.25f;
	constexpr float ResistCorruption = 0.6f;

	// --- Cloches ---
	constexpr float BellStunDuration = 2.f;

	// --- Vol de combat (phases 1-2) ---
	constexpr float HoverHeightAboveTarget = 200.f;       // (*) altitude de vol par rapport au centre de la cible
	constexpr float PreferredCombatDistance = 1100.f;     // (*) milieu de la plage de l'Estoc (600-1 800) et de la Lance (1 000-4 000)
	constexpr float OrbitDegreesPerSecond = 60.f;         // (*) vitesse de l'orbite autour de la cible

	// --- Estoc / Feinte ---
	constexpr float DashDistance = 1800.f;
	constexpr float DashDuration = 0.25f;
	constexpr float DashContactRadius = 150.f;            // (*) rayon de contact du dash (hors rayon de la cible)
	constexpr float DashAimLockLead = 0.15f;              // (*) la visée est figée (et l'éclat émis) 0,15 s avant le dash
	constexpr float DashMaxDescentSlope = 0.6f;           // (*) pente maximale vers le bas du dash en vol (composante Z normalisée)
	constexpr float DashMaxAscentSlope = 0.2f;            // (*) idem vers le haut
	constexpr float FeintMinDelay = 0.3f;
	constexpr float FeintMaxDelay = 0.9f;

	// --- Projectiles ---
	constexpr int32 FeatherCount = 24;
	constexpr int32 FeatherRows = 3;                      // (*) 3 rangées x 8 colonnes = 24 plumes
	constexpr float FeatherRowPitchStepDeg = 8.f;         // (*)
	constexpr float FeatherSpeed = 3000.f;
	constexpr float FeatherCorruption = 5.f;
	constexpr float FeatherRadius = 25.f;                 // (*)
	constexpr float FeatherLifetime = 1.6f;               // (*) 4 800 cm de portée, au-delà de la portée de 3 000 cm
	constexpr float LanceSpeed = 4000.f;
	constexpr float LanceReflectDamage = 300.f;
	constexpr float LanceReflectSpeed = 4000.f;           // (*) même vitesse au retour
	constexpr float LanceRadius = 40.f;                   // (*)
	constexpr float LanceLifetime = 1.6f;                 // (*) 6 400 cm de portée, au-delà de la portée de 4 000 cm
	constexpr float LanceAimLockLead = 0.25f;             // (*) la visée est figée 0,25 s avant le jet
	constexpr int32 ProjectilePoolPrewarm = 24;           // une rafale
	constexpr int32 ProjectilePoolMax = 96;               // (*) boss + 2 clones simultanés (3 x 24) + marge
	constexpr int32 ProjectilePoolPhase2Prewarm = 72;     // (*) pool porté à 72 dès le début de la transition vers la phase 2 (3 s d'invulnérabilité)

	// --- Halo noir ---
	constexpr float HaloCorruption = 20.f;
	constexpr float HaloFlashDuration = 0.35f;            // (*) flash visuel de l'éclatement

	// --- Piqué en spirale ---
	constexpr float SpiralDiveSpeed = 4500.f;             // (*) vitesse de plongée (celle du dragon)
	constexpr float SpiralCruiseAltitude = 1100.f;        // (*) altitude de départ du piqué au-dessus du point marqué
	constexpr float SpiralClimbMaxSpeed = 2200.f;         // (*) vitesse maximale de la montée préparatoire
	constexpr float SpiralHelixRadius = 250.f;            // (*)
	constexpr float SpiralHelixTurns = 2.f;               // (*)
	constexpr float DiveFlashDuration = 0.35f;
	constexpr float DiveImpactRadius = 400.f;
	constexpr float DivePredictionLead = 0.5f;            // (*) anticipation de la position de la cible
	constexpr float DeadStarPredictionLead = 0.8f;        // (*)
	constexpr float MaxPredictionOffset = 800.f;          // (*) plafond du décalage de prédiction
	constexpr float PlungeMarkAlignSpeed = 360.f;         // (*) deg/s : rotation du boss vers le point marqué pendant les plongeons
	constexpr float PlungeRecoverTime = 0.3f;             // (*) pause après le dernier impact du piqué

	// --- Chute de l'étoile morte ---
	constexpr int32 DeadStarPlungeCount = 3;
	constexpr float DeadStarMarkDuration = 0.8f;
	constexpr float DeadStarInterval = 0.9f;
	constexpr float DeadStarArcHeight = 700.f;            // (*) hauteur de l'arc du bond entre deux impacts
	constexpr float DeadStarVulnerableDuration = 4.f;
	constexpr float VulnerableDamageMultiplier = 1.5f;    // (*) même bonus que le dragon (v1.1)

	// --- Ombres jumelles ---
	constexpr int32 ShadowCount = 2;
	constexpr float ShadowLifetime = 20.f;
	constexpr float ShadowEchoDelay = 0.3f;
	constexpr float ShadowDamageScale = 0.3f;
	constexpr float ShadowSideOffset = 700.f;             // (*) écart latéral des clones par rapport à Nyxariel

	/** Canal de trace de mêlée du joueur (EMBMeleeHit = ECC_GameTraceChannel1, voir Config/DefaultEngine.ini). */
	constexpr ECollisionChannel MeleeHitChannel = ECC_GameTraceChannel1;
}

/**
 * Événement dommageable d'une attaque de Nyxariel, rejoué par ses ombres jumelles avec 0,3 s de retard.
 * AimPoint = point visé au moment de l'événement (cible, point marqué au sol...).
 */
struct FEMBSeraphEcho
{
	FGameplayTag AttackTag;
	int32 EventIndex = 0;
	FVector AimPoint = FVector::ZeroVector;
};

/**
 * Nyxariel, l'Aile Flétrie : Séraphe, boss 2 (GDD §9.2, référentiel §11.3). Séraphin corrompu à quatre ailes,
 * combat rapide à mi-distance : il punit la garde passive et la roulade précoce.
 *
 *  - Phases 1-2 : en vol (1 800 cm/s), orbite à ~11 m de sa cible ; phase 3 (25 % des PV) : au sol, vitesse x1,2.
 *  - Estoc / Feinte : dash balayé (UBTTask_EMBSeraphDash), parable.
 *  - Piqué en spirale / Chute de l'étoile morte : plongeons scriptés (UBTTask_EMBSeraphPlunge).
 *  - Ciseaux d'ailes : impacts génériques de la base (HitTimes). Pluie de plumes, Lance jetée, Halo noir, Ombres
 *    jumelles : événements programmés par ScheduleCustomEvents (timers de la base).
 *  - Projectiles : pool d'AEMBProjectile tenu par le boss. Clones : AEMBSeraphShadow.
 *  - Cloches de l'arène : AEMBSeraphArena (étourdit Nyxariel en vol, 2 s, une fois par cloche).
 *
 * CHOIX ASSUMÉS (*), à valider en playtest :
 *  - "vitesse x1,2" en phase 3 = vitesse de MARCHE au sol uniquement (800 -> 960 cm/s). Montages, télégraphes et recharges
 *    ne sont pas accélérés : les durées explicites du référentiel (0,8 s, 0,9 s...) restent exactes.
 *  - Orbite de combat à 1 100 cm en phases 1-2 : Nyxariel reste à mi-distance. La mêlée n'a lieu que lorsque le joueur
 *    s'approche (Ciseaux d'ailes, Halo noir), pendant les dashs (Estoc, Feinte) et pendant les plongeons.
 *
 * Le boss ne tourne jamais sur un Tick : timers de la base et tâches du Behavior Tree.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBSeraphBoss : public AEMBBossBase
{
	GENERATED_BODY()

public:
	AEMBSeraphBoss();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// --- AEMBBossBase ---
	virtual FGameplayTag GetDefeatedEventTag() const override;

	/**
	 * Phases 1-2 : aucune attaque n'est permise tant que Nyxariel n'est pas en vol (il décolle via la tâche de
	 * repositionnement). Ombres jumelles : refusées tant que des clones sont vivants.
	 */
	virtual bool IsAttackAllowed(const FEMBBossAttackRow& Row) const override;

	// --- État ---

	/** Nyxariel vole-t-il (phases 1-2, vivant) ? Faux en phase 3 : au sol définitivement. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Seraph")
	bool ShouldFly() const;

	/** Cible de combat : celle du contrôleur IA, sinon le joueur vivant le plus proche. */
	AActor* ResolveCombatTarget() const;

	float GetHoverHeightAboveTarget() const { return HoverHeightAboveTarget; }
	float GetPreferredCombatDistance() const { return PreferredCombatDistance; }

	/** Délai aléatoire de la Feinte (0,3 à 0,9 s), tiré par le FRandomStream serveur du boss. */
	float RollFeintDelay();

	/** Point au sol sous la position prédite de la cible (anticipation Lead, décalage plafonné). */
	FVector PredictTargetGroundPoint(const AActor* Target, float Lead) const;

	/** Origine des projectiles : socket ProjectileSocketName, sinon devant le boss. */
	FVector GetProjectileOrigin() const;

	// --- Coups ---

	/**
	 * Gabarit de coup d'une ligne de la table, mis à l'échelle (clones : 0,3) et complété de la Corruption propre à
	 * l'attaque (Pluie de plumes 5, Halo noir 20). Instigateur = ce boss.
	 */
	FEMBHitData MakeSeraphHitData(const FEMBBossAttackRow& Row, float DamageScale = 1.f);

	/**
	 * Contact d'un dash : touche, une seule fois chacun, les joueurs dont la distance au segment From-To est inférieure
	 * à Radius + rayon de leur capsule. Test sur le segment (et non sur le point d'arrivée) : à 7 200 cm/s un pas
	 * de 120 cm saute sinon un joueur. Retourne vrai si un joueur a paré le coup.
	 */
	bool ResolveDashContact(const FVector& From, const FVector& To, float Radius, const FEMBHitData& HitTemplate, TSet<TWeakObjectPtr<AActor>>& InOutAlreadyHit);

	// --- Projectiles ---

	/** Éventail de 24 plumes (3 rangées x 8) centré sur AimPoint, ouverture Row.HalfAngleDeg. Serveur. */
	void LaunchFeatherFan(const FEMBBossAttackRow& Row, const FVector& Origin, const FVector& AimPoint, float DamageScale);

	/** Lance parable (renvoi en cas de parade parfaite). DamageScale < 1 (clone) : lance non parable. Serveur. */
	void LaunchLance(const FEMBBossAttackRow& Row, const FVector& Origin, const FVector& AimPoint, float DamageScale);

	// --- Ombres jumelles ---

	/** Transmet l'événement aux clones vivants : ils le rejoueront ShadowEchoDelay plus tard. Serveur. */
	void BroadcastShadowEcho(const FEMBSeraphEcho& Echo);

	int32 GetAliveShadowCount() const;

	/** Éclat de la pointe de la lance (signal d'esquive de la Feinte, éclat final de l'Estoc). Cosmétique. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_TipFlash(FGameplayTag AttackTag, float Lead);

protected:
	// --- Hooks de la base ---
	virtual bool ScheduleCustomEvents(const FEMBBossAttackRow& Row, float TimeScale, float& InOutLastEventTime) override;
	virtual void OnTimedHitExecuted(const FEMBBossAttackRow& Row, int32 HitIndex, const TArray<AActor*>& Landed) override;
	virtual void OnPhaseEnteredServer(int32 NewPhase, int32 OldPhase) override;
	virtual void OnDefeatedServer() override;
	virtual void OnEncounterReset() override;

	/** Mise en scène : éclat de la pointe (tous les clients). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Seraph", meta = (DisplayName = "On Tip Flash"))
	void BP_OnTipFlash(FGameplayTag AttackTag, float Lead);

	/** Altitude de vol par rapport au centre de la cible (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph|Vol", meta = (ClampMin = "0.0"))
	float HoverHeightAboveTarget = EMBSeraphDefaults::HoverHeightAboveTarget;

	/** Distance d'orbite préférée autour de la cible (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph|Vol", meta = (ClampMin = "100.0"))
	float PreferredCombatDistance = EMBSeraphDefaults::PreferredCombatDistance;

	/** Socket d'où partent plumes et lance (main, aile). Absent : devant le boss. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph|Projectiles")
	FName ProjectileSocketName = TEXT("ProjectileOrigin");

	/** Classe de projectile (Blueprint dérivé d'AEMBProjectile : mesh, VFX de plume et de lance). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph|Projectiles")
	TSubclassOf<AEMBProjectile> ProjectileClass;

	/** Classe des clones (Blueprint dérivé d'AEMBSeraphShadow). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph|Ombres")
	TSubclassOf<AEMBSeraphShadow> ShadowClass;

protected:
	UFUNCTION()
	void HandlePhaseTransitionStarted(int32 PendingPhase);

	UFUNCTION()
	void HandleEncounterEnded();

private:
	// --- Événements programmés (timers de la base) ---
	void ExecuteFeatherRain(FGameplayTag AttackTag);
	void LockLanceAim();
	void ExecuteLanceThrow(FGameplayTag AttackTag);
	void ExecuteBlackHalo(FGameplayTag AttackTag);
	void ExecuteSummonShadows(FGameplayTag AttackTag);

	// --- Pool de projectiles (serveur) ---
	AEMBProjectile* AcquireProjectile();
	AEMBProjectile* SpawnPooledProjectile();
	void PrewarmProjectilePool(int32 TargetCount);
	void DestroyProjectilePool();

	void DissipateAllShadows();

	/**
	 * Pool de projectiles réutilisés (voir AEMBProjectile). Un projectile libre = IsProjectileActive() faux.
	 * Coût : PrewarmCount acteurs créés au BeginPlay, jamais détruits avant la fin du boss ; croissance à la demande
	 * jusqu'à ProjectilePoolMax (au-delà, le tir est ignoré plutôt que de saturer la bande passante).
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AEMBProjectile>> ProjectilePool;

	TArray<TWeakObjectPtr<AEMBSeraphShadow>> Shadows;

	/** Point visé par la Lance, figé avant le jet (la direction que le joueur lit est celle du tir). */
	FVector LockedLanceAimPoint = FVector::ZeroVector;
};
