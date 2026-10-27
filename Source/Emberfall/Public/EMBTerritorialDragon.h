// Copyright ADWEB Conseil. Tous droits réservés.

/*
 * ===============================================================================================
 * STRUCTURE DU BEHAVIOR TREE ATTENDU : BT_Sylvadrac (Blackboard : BB_Sylvadrac)
 * ===============================================================================================
 * Contrôleur : AEMBBossAIController (celui de la base) ou une classe fille Blueprint qui y assigne BT_Sylvadrac et
 * BB_Sylvadrac. Mêmes clés que le Colosse (TargetActor, TargetDistance, TargetAngle, CurrentPhase, bIsFlying, bIsStunned,
 * bIsTransitioning, bIsDead, bIsVulnerable, bCataclysmDue, ThreatCount, SelectedAttackTag, SelectedAttackKind,
 * SelectedAttackDuration) PLUS trois clés écrites par le dragon lui-même (machine d'états du territoire) :
 *   bTerritoryAggressive (Bool), bReturningToNest (Bool), NestLocation (Vector).
 *
 * Root
 * └─ Selector "Racine"                       [Service : BTService_EMBUpdateCombatContext, MaxTargetDistance = 30000]
 *    ├─ 1. Sequence "Mort"                   [Deco BB bIsDead == true, aborts Both]          -> Wait long
 *    ├─ 2. Sequence "Étourdi"                [Deco BB bIsStunned == true, aborts Both]       -> Wait 0,25 s
 *    ├─ 3. Sequence "Retour au nid"          [Deco BB bReturningToNest == true, aborts Both]
 *    │     ├─ MoveTo (clé NestLocation, rayon 600)
 *    │     └─ Wait 0,5 s                       (la machine d'états repasse à Idle à l'arrivée et régénère)
 *    ├─ 4. Sequence "Agressif"               [Deco BB bTerritoryAggressive == true, aborts Both]
 *    │     └─ Selector
 *    │          ├─ Sequence "Combat"          [Deco BB TargetActor is Set, aborts Lower Priority]
 *    │          │     ├─ BTTask_EMBSelectAttack          (échoue si rien de valide, ou si le dragon retombe)
 *    │          │     ├─ Selector "Exécution par type"
 *    │          │     │    └─ [SelectedAttackKind == 0 (Melee) ou 3 (Area)]
 *    │          │     │         Wait Blackboard Time (clé SelectedAttackDuration)  (impacts programmés par le dragon)
 *    │          │     └─ Wait 0,5 à 0,9 s (aléatoire)   (récupération : fenêtre de punition)
 *    │          └─ Sequence "Repositionnement"          MoveTo (clé TargetActor, rayon 500) puis Wait 0,3 s
 *    └─ 5. Sequence "Veille"                 Wait 1 s     (au nid, rugissement et décompte gérés en C++)
 *
 * Machine d'états (AEMBTerritorialDragon::TickTerritory, timer 0,5 s) :
 *   Idle       -> un joueur vivant entre dans le territoire (30 000 cm autour du nid) : Warned + rugissement
 *                 (Attack.Sylvadrac.Roar, montage RoarMontage, 30 s de recharge entre deux rugissements) ;
 *   Warned     -> 20 s de présence continue : Aggressive ; le joueur sort avant : Idle ;
 *   Aggressive -> BeginEncounter (échelle coop), le BT attaque ; plus aucun joueur dans le territoire pendant 8 s,
 *                 ou dragon à plus de 3 000 cm au-delà du territoire : Returning ;
 *   Returning  -> BT : MoveTo le nid ; arrivé à moins de 800 cm : Idle. Hors Aggressive, PV régénérés (2 %/s).
 * Un coup reçu d'un joueur provoque l'agression immédiate (même pendant l'avertissement ou le retour).
 *
 * Attaques (Data/DT_SylvadracAttacks.csv, à assigner à AttackTable dans le Blueprint) :
 *   Bite (Melee, morsure 220, parable, empoisonne) ; LowDive (Melee, piqué bas 300, bond au sol, non parable) ;
 *   PoisonCloud (Area, nuage toxique 8 s, 20 dégâts/s, marqueur au sol visible pendant le télégraphe).
 * ===============================================================================================
 */

#pragma once

#include "CoreMinimal.h"
#include "EMBBossBase.h"
#include "EMBCreatureTypes.h"
#include "EMBTerritorialDragon.generated.h"

class AEMBCreatureSpawner;
class APlayerState;
class UAnimMontage;
class UDataTable;

/** Etat de la machine d'états du territoire du Sylvadrac. */
UENUM(BlueprintType)
enum class EEMBTerritoryState : uint8
{
	/** Au nid, personne dans le territoire. */
	Idle,
	/** Un joueur est entré : rugissement émis, décompte de 20 s. */
	Warned,
	/** Combat : le Behavior Tree attaque. */
	Aggressive,
	/** Retour au nid (joueurs partis, ou laisse dépassée). */
	Returning
};

/** Nuage toxique persistant (données serveur uniquement, jamais répliquées). */
struct FEMBPoisonCloud
{
	FVector Center = FVector::ZeroVector;
	float Radius = 0.f;
	double EndTime = 0.0;
};

/**
 * Sylvadrac, dragon sauvage territorial de la forêt profonde (GDD §7.3, référentiel §16) : mini-boss du monde, à une
 * seule phase, qui dérive de AEMBBossBase (posture, points faibles, attaques chronométrées, dégâts de zone).
 *
 * Budget serveur de 40 créatures : le Sylvadrac n'y est PAS compté. C'est un boss unique (son spawner est limité à
 * MaxAlive = 1, au plus un exemplaire vivant par territoire), il n'est pas une AEMBCreatureBase et ne s'inscrit pas
 * dans UEMBCreatureSignificance ; le budget protège de la multiplication des petites créatures, pas des boss.
 *
 * Ce que la base ne fait pas :
 *  - territoire : rugissement à l'entrée d'un joueur, attaque après 20 s de présence, retour au nid et régénération ;
 *  - poison : dégâts continus de 20/s (ticks de 0,25 s, ni esquive ni garde : bDamageOverTime), appliqué par la
 *    morsure ; nuage toxique persistant de 8 s qui entretient le poison de quiconque s'y tient ;
 *  - piqué bas : bond au sol vers la cible, impact en sphère ;
 *  - défaite NON définitive : GetDefeatedEventTag() renvoie un tag INVALIDE. Conséquences voulues, vérifiées dans le
 *    code existant : UEMBProgressionSubsystem::NotifyWorldEvent ignore un tag invalide (rien n'est enregistré comme
 *    événement monde, donc rien n'est sauvegardé), CheckDefeatedFromSave et la boucle de UEMBSaveSubsystem testent
 *    IsValid() avant ApplyDefeatedFromSave : le Sylvadrac n'est jamais marqué "vaincu" et un nouveau dragon peut
 *    apparaître. L'XP (20 000) et le déclencheur Event.Creature.Killed.Sylvadrac sont distribués ici, à chaque mort,
 *    aux joueurs à <= ParticipantRadius (10 000 cm) ; la carcasse SylvadracCarcass est posée via SpawnDynamicNode ;
 *  - réapparition 144 min (3 journées de jeu) après la mort, non sauvegardée : portée par un AEMBCreatureSpawner (celui
 *    qui a créé le dragon, ou un spawner créé à sa mort s'il avait été posé à la main).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBTerritorialDragon : public AEMBBossBase
{
	GENERATED_BODY()

public:
	AEMBTerritorialDragon();

	// --- Clés du Blackboard propres au territoire (en plus de celles de AEMBBossAIController) ---
	static const FName BBKey_NestLocation;
	static const FName BBKey_TerritoryAggressive;
	static const FName BBKey_ReturningToNest;

	virtual void PreInitializeComponents() override;
	virtual void BeginPlay() override;

	/** Un coup d'un joueur provoque l'agression immédiate, puis la base résout le coup. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	/**
	 * Rencontre active seulement en état Aggressive (et rencontre engagée, voir la base) : un dragon qui rugit ou
	 * dort au nid ne bloque pas l'autosave.
	 */
	virtual bool IsEncounterActive() const override;

	/**
	 * La rencontre (et la mise à l'échelle coop des PV) ne démarre qu'en état Aggressive : les appels de la base ou du
	 * contrôleur pendant l'avertissement ou au nid sont ignorés. EnterAggressive fixe l'état puis rappelle la base.
	 */
	virtual void BeginEncounter() override;

	/** Tag INVALIDE : le Sylvadrac n'est jamais enregistré comme boss vaincu (voir la description de la classe). */
	virtual FGameplayTag GetDefeatedEventTag() const override;

	/** XP des participants (20 000, lue dans DT_Creatures). Surchargée aussi pour éviter l'avertissement du tag inconnu. */
	virtual int64 GetDefeatXP() const override;

	/** Appelé par le spawner avant FinishSpawning. */
	void SetOwningSpawner(AEMBCreatureSpawner* Spawner);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Territory")
	EEMBTerritoryState GetTerritoryState() const { return TerritoryState; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Territory")
	FVector GetNestLocation() const { return NestLocation; }

protected:
	virtual bool ScheduleCustomEvents(const FEMBBossAttackRow& Row, float TimeScale, float& InOutLastEventTime) override;
	virtual void OnTimedHitExecuted(const FEMBBossAttackRow& Row, int32 HitIndex, const TArray<AActor*>& Landed) override;
	virtual void OnDefeatedServer() override;
	virtual void OnEncounterReset() override;

	// --- Configuration (Blueprint du Sylvadrac) ---

	/** Table des créatures (ligne "Sylvadrac" : PV, posture, vitesses, XP, carcasse, rayon du territoire). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory|Data")
	TSoftObjectPtr<UDataTable> CreatureTable;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory|Data")
	FName CreatureRowName = FName(TEXT("Sylvadrac"));

	/** Rayon du territoire (cm) autour du nid. Remplacé par LeashRadius de la ligne de données si elle est trouvée. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory", meta = (ClampMin = "1000.0"))
	float TerritoryRadius = EMBSylvadracDefaults::TerritoryRadius;

	/** Secondes de présence continue d'un joueur avant l'attaque. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory", meta = (ClampMin = "0.0"))
	float AggressionDelay = EMBSylvadracDefaults::AggressionDelay;

	/** Délai (s) entre la mort et la réapparition : 3 journées de jeu = 144 min. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory", meta = (ClampMin = "1.0"))
	float RespawnDelaySeconds = EMBSylvadracDefaults::RespawnDelay;

	/** Montage du rugissement d'avertissement. Optionnel (le télégraphe Multicast_Telegraph part dans tous les cas). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Territory|Montages")
	TObjectPtr<UAnimMontage> RoarMontage;

private:
	// Territoire
	void TickTerritory();
	void BeginWarning(double Now);
	void EnterAggressive(AActor* Provoker);
	void EnterReturning();
	void RegenerateHealth();
	void PlayRoar();
	void ApplyTerritorySpeed();
	void PushTerritoryToBlackboard();
	float PlanarDistanceToNest(const FVector& Location) const;
	AActor* GetCombatTargetActor() const;

	// Poison
	void ApplyPoison(AActor* Victim);
	void EnsurePoisonTimer();
	void TickPoison();
	void SpawnPoisonCloud(FGameplayTag AttackTag, FVector Center);
	FVector ComputeCloudCenter() const;

	// Piqué bas
	void LaunchLowDive(FGameplayTag AttackTag, float FlightDuration);
	void ResolveLowDiveImpact(FGameplayTag AttackTag);

	// Mort et réapparition
	void AwardParticipantExperience();
	void SpawnCarcass();
	void ScheduleRespawn();

	EEMBTerritoryState TerritoryState = EEMBTerritoryState::Idle;

	/** Ligne de données (copie) : XP, carcasse, vitesses. Valeurs de repli (EMBSylvadracDefaults) si la table manque. */
	FEMBCreatureRow CachedRow;
	bool bHasRow = false;

	FVector NestLocation = FVector::ZeroVector;
	FRotator NestRotation = FRotator::ZeroRotator;
	double WarnStartTime = 0.0;
	double LastRoarTime = -1000.0;
	double EmptySince = -1.0; // heure monde du début de l'absence de joueur en combat (-1 : aucune)

	TMap<TWeakObjectPtr<AActor>, double> PoisonEndTimes; // victime -> heure monde de fin du poison
	TArray<FEMBPoisonCloud> PoisonClouds;

	TWeakObjectPtr<AEMBCreatureSpawner> OwningSpawner;

	/** Joueurs ayant infligé des dégâts -> heure monde du dernier coup (XP à la mort, comme les créatures). */
	TMap<TWeakObjectPtr<APlayerState>, double> Contributors;

	FTimerHandle TerritoryTimerHandle;
	FTimerHandle PoisonTimerHandle;
};
