// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayTagAssetInterface.h"
#include "GameplayTagContainer.h"
#include "Engine/StreamableManager.h"
#include "EMBCombatTypes.h"
#include "EMBHitReceiver.h"
#include "EMBDragonTypes.h"
#include "EMBBossBase.generated.h"

class UAnimMontage;
class UDataTable;
class UEMBStatsComponent;
class UNiagaraComponent;
class UNiagaraSystem;

/**
 * Ligne de données d'attaque générique d'un boss. Le format du dragon (FEMBDragonAttackRow) sert de format
 * commun : le CSV du dragon reste importable tel quel et les CSV du Colosse et de Nyxariel utilisent les mêmes colonnes.
 */
using FEMBBossAttackRow = FEMBDragonAttackRow;

/** Constantes partagées par les boss et leurs arènes. */
namespace EMBBossDefaults
{
	/** Intervalle (s) de recherche du boss par une arène tant qu'il n'est pas encore présent dans le monde. */
	constexpr float ArenaBossSearchInterval = 0.5f;

	/** Rencontre abandonnée : durée (s) sans aucun joueur vivant dans l'arène avant la remise à zéro du boss. */
	constexpr float WipeResetDelay = 30.f;

	/** Période (s) de la vérification "aucun joueur vivant dans l'arène". */
	constexpr float WipeCheckInterval = 1.f;
}

/** Changement de phase effectif (fin du rugissement). Diffusé sur le serveur ET sur les clients (RepNotify). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnBossPhaseChanged, int32, NewPhase, int32, OldPhase);

/** Début d'une transition de phase (rugissement, invulnérabilité). PendingPhase = phase qui va suivre. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnBossPhaseTransitionStarted, int32, PendingPhase);

/** Posture brisée / fin d'étourdissement. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnBossStunChanged, bool, bStunned);

/** Mort du boss : EventTag = Event.Boss.Killed.* du boss. Serveur uniquement (la progression est autoritaire). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnBossDefeated, FGameplayTag, EventTag, AActor*, Killer);

/** Fin de la rencontre : boss vaincu en combat, ou marqué vaincu par une sauvegarde. Serveur et clients (RepNotify). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMBOnBossEncounterEnded);

/** Zone de feu persistante (souffle aérien). Données serveur uniquement, jamais répliquées. */
struct FEMBBossFireZone
{
	FVector Center = FVector::ZeroVector;
	float Radius = 0.f;
	double EndTime = 0.0;
	float DamagePerSecond = 0.f;
};

/** Menace accumulée par un joueur (dégâts infligés au boss, avec décroissance exponentielle). */
struct FEMBBossThreatEntry
{
	float Value = 0.f;
	double LastUpdateTime = 0.0;
};

/**
 * Base commune des boss d'Emberfall (référentiel §11.1) : Colosse d'Ossivaire, Nyxariel, Vaelthyrax.
 *
 * Factorise tout ce qui est générique : stats et posture, mise à l'échelle coop, phases par seuils de PV avec
 * invulnérabilité de transition et enragé optionnel, résistances par type de dégâts, points faibles par os
 * (permanents ou conditionnels), attaques chronométrées (BeginTimedAttack), recharges, historique
 * anti-répétition, dégâts de zone et de cône, brûlure, zones de feu, vol scripté, rencontre
 * (BeginEncounter / IsEncounterActive), défaite (XP et événement monde, ApplyDefeatedFromSave).
 *
 * COMMENT DÉRIVER UN BOSS (guide pour Dev E)
 *  1. Dans le constructeur de la classe fille : ConfigureBossStats(PV, posture, régén., délai), puis régler les
 *     membres protégés (PhaseHealthThresholds, PhaseTransitionDuration, StunDuration, EnrageFromPhase et
 *     multiplicateurs, DamageResistances, WeakPointMultipliers, vitesses du CharacterMovement...).
 *  2. Surcharger GetDefeatedEventTag() (Event.Boss.Killed.*) : l'XP est lue automatiquement dans
 *     UEMBProgressionSettings (GuardianXP / SeraphXP / DragonXP) selon ce tag.
 *  3. Fournir une DataTable de FEMBBossAttackRow dans AttackTable (même format que DT_DragonAttacks.csv).
 *  4. Hooks serveur optionnels (voir la section protégée) : OnPhaseEnteredServer, OnPoiseDamageTaken,
 *     ScheduleCustomEvents, OnTimedHitExecuted, GetChainedAttackTag, OnAttackCancelled, OnDefeatedServer ;
 *     et publics : IsAttackAllowed, GetAttackWeightModifier, IsScriptedAttackDue.
 *  5. Utiliser AEMBBossAIController (ou une classe fille) avec un Behavior Tree dont la tâche de sélection, le
 *     service de contexte et le décorateur de phase travaillent directement sur cette base.
 *
 * Architecture réseau : serveur autoritaire. Toute la logique (phases, dégâts, recharges) ne s'exécute que si
 * HasAuthority(). Les clients reçoivent :
 *  - les PV / la posture via UEMBStatsComponent (répliqué) ;
 *  - la phase (RepNotify), l'étourdissement, la transition, le vol et la mort via des propriétés répliquées ;
 *  - les montages et les VFX via des NetMulticast (cosmétiques, ignorés sur serveur dédié quand c'est possible).
 * Aucun Tick : tout est piloté par des événements, des timers et les tâches du Behavior Tree.
 */
UCLASS(Abstract, Blueprintable)
class EMBERFALL_API AEMBBossBase : public ACharacter, public IEMBHitReceiver, public IGameplayTagAssetInterface
{
	GENERATED_BODY()

public:
	AEMBBossBase();

	// --- AActor / ACharacter ---
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// --- IEMBHitReceiver ---
	/** Serveur uniquement. Résistances, points faibles par os, invulnérabilité de transition, critique sur boss étourdi. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	// --- IGameplayTagAssetInterface ---
	virtual void GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const override;

	// ------------------------------------------------------------------------------------------
	// Défaite et rencontre
	// ------------------------------------------------------------------------------------------

	/** Tag d'événement monde diffusé à la mort (Event.Boss.Killed.Guardian / .Seraph / .Dragon). À surcharger. */
	virtual FGameplayTag GetDefeatedEventTag() const PURE_VIRTUAL(AEMBBossBase::GetDefeatedEventTag, return FGameplayTag(););

	/** XP accordée à chaque participant : lue dans UEMBProgressionSettings selon GetDefeatedEventTag(). */
	virtual int64 GetDefeatXP() const;

	/**
	 * Marque le boss comme déjà vaincu (monde chargé d'une sauvegarde où l'événement de défaite est enregistré).
	 * Serveur uniquement, idempotent. Le boss reste en place mais dormant (caché, sans collision, sans IA) ;
	 * aucun XP n'est redonné et OnBossDefeated n'est PAS rediffusé. Appelable par le système de sauvegarde
	 * (restauration asynchrone possiblement postérieure au BeginPlay) ; le BeginPlay fait aussi le contrôle.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Boss")
	void ApplyDefeatedFromSave();

	/**
	 * Vrai tant qu'une rencontre est en cours : engagée, boss vivant ET au moins un joueur vivant à moins de
	 * ArenaRadius du boss. Sans joueur vivant dans l'arène pendant WipeResetDelay (30 s), la rencontre est abandonnée
	 * (PV remis au maximum, phase conservée, échelle coop recalculée à la prochaine rencontre) : la sauvegarde n'est pas bloquée.
	 */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsEncounterActive() const;

	/** Démarre le combat (mise à l'échelle coop des PV). Idempotent, appelé à la première détection d'un joueur. */
	void BeginEncounter();

	// ------------------------------------------------------------------------------------------
	// Délégués
	// ------------------------------------------------------------------------------------------

	/** Abonnement de l'arène (piliers, ponts, lave). Déclenché sur serveur et clients. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Boss")
	FEMBOnBossPhaseChanged OnPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Boss")
	FEMBOnBossPhaseTransitionStarted OnPhaseTransitionStarted;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Boss")
	FEMBOnBossStunChanged OnStunChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Boss")
	FEMBOnBossDefeated OnBossDefeated;

	/** Rencontre terminée (mort au combat OU boss déjà vaincu d'après la sauvegarde) : l'arène passe en état final calme. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Boss")
	FEMBOnBossEncounterEnded OnEncounterEnded;

	// ------------------------------------------------------------------------------------------
	// État (lecture)
	// ------------------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	int32 GetCurrentPhase() const { return CurrentPhase; }

	/** Nombre de phases du boss = nombre de seuils + 1. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	int32 GetPhaseCount() const { return PhaseHealthThresholds.Num() + 1; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsFlying() const { return bIsFlying; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsStunned() const { return bIsStunned; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsTransitioning() const { return bIsTransitioning; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsVulnerable() const { return bIsVulnerable; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsDead() const { return bIsDead; }

	/** Vrai à partir de la phase EnrageFromPhase (0 : jamais enragé). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	bool IsEnraged() const { return EnrageFromPhase > 0 && CurrentPhase >= EnrageFromPhase; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	UEMBStatsComponent* GetStatsComponent() const { return StatsComponent; }

	/** Vitesse des montages : 1,0 ; EnragedPlayRate une fois enragé. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	float GetAttackPlayRate() const;

	/** Multiplicateur des télégraphes codés et des impacts programmés : 1,0 ; EnragedTelegraphMultiplier une fois enragé. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	float GetTelegraphMultiplier() const;

	/** Multiplicateur des recharges : 1,0 ; EnragedCooldownMultiplier une fois enragé. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Boss")
	float GetCooldownMultiplier() const;

	/** Tag Boss.Phase.N correspondant (N de 1 à 3). */
	static FGameplayTag GetPhaseTag(int32 Phase);

	// ------------------------------------------------------------------------------------------
	// Données d'attaque, recharges et historique (serveur)
	// ------------------------------------------------------------------------------------------

	const FEMBBossAttackRow* FindAttackRow(const FGameplayTag& AttackTag) const;
	const TArray<const FEMBBossAttackRow*>& GetAttackRows() const { return AttackRows; }

	bool IsAttackOnCooldown(const FGameplayTag& AttackTag) const;
	float GetCooldownRemaining(const FGameplayTag& AttackTag) const;

	/** Enregistre l'attaque choisie : historique des 2 dernières attaques + démarrage de la recharge. */
	void RegisterAttackSelected(const FEMBBossAttackRow& Row);

	const FGameplayTag& GetLastAttackTag() const { return LastAttackTag; }
	const FGameplayTag& GetPreviousAttackTag() const { return PreviousAttackTag; }

	/** Filtre additionnel de la sélection (ex. Résonance exclue sans obélisque). Défaut : toujours permise. */
	virtual bool IsAttackAllowed(const FEMBBossAttackRow& Row) const { return true; }

	/** Multiplicateur de poids additionnel de la sélection (ex. rafale d'ailes x3 si groupe). Défaut : 1,0. */
	virtual float GetAttackWeightModifier(const FEMBBossAttackRow& Row) const { return 1.f; }

	/** Une attaque scriptée est-elle due (Cataclysm du dragon) ? Poussé dans le Blackboard (bCataclysmDue). */
	virtual bool IsScriptedAttackDue() const { return false; }

	// ------------------------------------------------------------------------------------------
	// Exécution des attaques
	// ------------------------------------------------------------------------------------------

	/**
	 * Serveur : joue un montage chez tous (NetMulticast) et retourne sa durée réelle en secondes
	 * (longueur / vitesse). PlayRate <= 0 : vitesse de phase. Retourne 0 si montage nul ou client.
	 */
	float PlayAttackMontage(UAnimMontage* Montage, float PlayRate = -1.f, FName StartSection = NAME_None);

	/**
	 * Attaques non latentes (mêlée, saisie, zone, pluie de braises, attaques spéciales d'une classe fille) : joue le
	 * montage, programme les impacts par timers et retourne la durée totale à attendre (écrite dans le Blackboard
	 * par SelectAttack). Les instants d'impact sont mis à l'échelle par GetTelegraphMultiplier().
	 */
	float BeginTimedAttack(const FEMBBossAttackRow& Row);

	/** Annule les impacts programmés (étourdissement, transition, mort, abandon du BT). */
	void CancelTimedAttack(bool bStopMontage);

	/** Étourdit le boss (posture brisée, impact de charge...) : stoppe l'attaque en cours, joue StunMontage. Serveur. */
	void BeginStun(float Duration);

	/** Passe en vol (MOVE_Flying) ou revient au sol (MOVE_Falling puis atterrissage automatique). Serveur. */
	void SetFlying(bool bNewFlying);

	/**
	 * Déplace le boss vers Destination à vitesse constante (vol scripté : décollage, piqué, strafe).
	 * Retourne vrai si arrivé ; bOutBlocked vrai si le balayage a heurté la géométrie.
	 */
	bool StepFlightToward(const FVector& Destination, float Speed, float DeltaSeconds, bool& bOutBlocked);

	/**
	 * Course scriptée au sol (charge) : avance de Speed*DeltaSeconds le long de Direction (à plat). Le déplacement est
	 * balayé contre le décor SEULEMENT (WorldStatic et WorldDynamic) : les joueurs ne bloquent pas la course.
	 * Retourne vrai si un obstacle bloque ; OutBlockingHit contient alors l'impact.
	 */
	bool StepGroundCharge(const FVector& Direction, float Speed, float DeltaSeconds, FHitResult& OutBlockingHit);

	/** Fenêtre de vulnérabilité au sol (après le piqué : 2,5 s). */
	void BeginVulnerableWindow(float Duration);

	/**
	 * Active (ou non) les points faibles conditionnels pendant Duration secondes (0 = jusqu'à nouvel ordre).
	 * Colosse : noyau exposé 3 s après le Séisme.
	 */
	void SetConditionalWeakPointsActive(bool bActive, float Duration = 0.f);
	bool AreConditionalWeakPointsActive() const { return bConditionalWeakPointsActive; }

	/** Le boss cesse (true) ou reprend (false) de suivre sa cible du regard : engagement d'une attaque. */
	void SetFacingLocked(bool bLocked);
	bool IsFacingLocked() const { return bFacingLocked; }

	/** Fait pivoter le boss vers un cap (yaw, degrés) à vitesse angulaire constante. */
	void RotateYawToward(float TargetYawDeg, float DeltaSeconds, float DegreesPerSecond);

	// ------------------------------------------------------------------------------------------
	// Dégâts de zone (serveur)
	// ------------------------------------------------------------------------------------------

	/** Gabarit de coup rempli depuis une ligne de la DataTable (Instigator = ce boss). */
	FEMBHitData MakeHitData(const FEMBBossAttackRow& Row);

	/**
	 * Touche les joueurs dans une sphère (distance - rayon de capsule, filtrage Pawn joueur).
	 * bRequireLineOfSight : un obstacle entre Center et le joueur annule le coup (Cataclysm).
	 * bHorizontalOnly : distance mesurée à plat (cylindre infini en hauteur), pour les ondes au sol.
	 * InOutAlreadyHit (optionnel) : joueurs déjà traités par une attaque à effet continu (charge) ; ils sont ignorés
	 * et les joueurs traités y sont ajoutés (une seule fois par joueur).
	 * Retourne les joueurs effectivement touchés (ni esquivé, ni paré, ni ignoré).
	 */
	TArray<AActor*> ApplyAreaDamage(const FVector& Center, float Radius, const FEMBHitData& HitTemplate, bool bRequireLineOfSight = false, TSet<TWeakObjectPtr<AActor>>* InOutAlreadyHit = nullptr, bool bHorizontalOnly = false);

	/** Idem pour un cône (demi-angle en degrés, longueur en cm) ; marge angulaire = rayon de capsule de la cible. */
	TArray<AActor*> ApplyConeDamage(const FVector& Origin, const FVector& Direction, float HalfAngleDeg, float Length, const FEMBHitData& HitTemplate, bool bRequireLineOfSight = false, bool bHorizontalOnly = false);

	/** Brûlure 15/s pendant 4 s, rafraîchie (non cumulée) à chaque nouvelle application. */
	void ApplyBurn(AActor* Target);

	/** Zone de feu persistante (souffle aérien : 8 s, 20/s). */
	void AddFireZone(const FVector& Center, float Radius, float Duration, float DamagePerSecond);

	/** Position de la gueule (socket MouthSocketName, sinon estimation devant le boss). */
	FVector GetBreathOrigin() const;

	/** Projette un point sur le sol (trace verticale). Retourne le point d'entrée si rien n'est touché. */
	FVector ProjectToGround(const FVector& Location) const;

	// ------------------------------------------------------------------------------------------
	// Contexte de combat
	// ------------------------------------------------------------------------------------------

	/** Distance à plat (cm) et angle signé (degrés, positif = cible à droite) du boss vers l'acteur. */
	void GetDistanceAndAngleTo(const AActor* Target, float& OutDistance, float& OutSignedAngleDeg) const;

	int32 CountPlayersWithin(float Radius) const;

	/** Rayon de l'arène (cm) : maintien de la rencontre, échelle coop, déclenchement. */
	float GetArenaRadius() const { return ArenaRadius; }

	/** Joueurs vivants possédant un Pawn (itération des PlayerControllers, 4 maximum). */
	static void CollectAlivePlayerPawns(const UWorld* World, TArray<APawn*>& OutPawns);

	void AddThreat(AActor* Source, float Amount);

	/** Menace récente normalisée d'un joueur (décroissance de demi-vie 8 s). */
	float GetThreatScore(AActor* Player) const;

	// ------------------------------------------------------------------------------------------
	// Réseau : effets cosmétiques
	// ------------------------------------------------------------------------------------------

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_PlayMontage(UAnimMontage* Montage, float PlayRate, FName StartSection);

	// Réseau : les montages et le souffle (état visible durable : un souffle non arrêté resterait affiché) sont
	// Reliable. Tout ce qui est ponctuel et purement cosmétique est Unreliable (perte tolérée, pas de blocage du canal).
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_StopMontages(float BlendOutTime);

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_StartBreathVFX(UNiagaraSystem* System, FName SocketName);

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_StopBreathVFX();

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_SpawnVFXAtLocation(UNiagaraSystem* System, FVector Location, FRotator Rotation, FVector Scale);

	/** Fait apparaître localement le VFX de zones de feu (envoyé à la pose de chaque zone : fréquence faible, donc Reliable). */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_SpawnFireZoneVFX(UNiagaraSystem* System, const TArray<FVector>& Locations);

	/** Télégraphe visuel (gorge incandescente, patte levée...) : appelle l'événement Blueprint chez tous. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_Telegraph(FGameplayTag AttackTag, float Duration);

	/** Marqueur au sol (ombre du piqué, cercle de braises, foudre, onde). bFlash : flash final avant l'impact. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_ShowGroundMarker(FVector Location, float Radius, float Duration, bool bFlash);

	/** VFX des zones de feu persistantes (souffle aérien). */
	UNiagaraSystem* ResolveFireZoneVFX() const { return ResolveVFX(FireZoneVFX); }

	UNiagaraSystem* ResolveVFX(const TSoftObjectPtr<UNiagaraSystem>& Soft) const;
	UAnimMontage* ResolveMontage(const TSoftObjectPtr<UAnimMontage>& Soft) const;

protected:
	// ------------------------------------------------------------------------------------------
	// Hooks pour les classes filles (serveur sauf mention contraire)
	// ------------------------------------------------------------------------------------------

	/** Une phase vient d'être effectivement franchie (fin du rugissement). Ex. dragon : programme le premier Cataclysm. */
	virtual void OnPhaseEnteredServer(int32 NewPhase, int32 OldPhase) {}

	/** Le boss vient de subir Amount de dégâts de posture (hors étourdissement). Ex. Colosse : interrompt la Résonance. */
	virtual void OnPoiseDamageTaken(float Amount, AActor* InInstigator) {}

	/**
	 * Attaque spéciale d'une classe fille : programme ses propres événements avec AddAttackTimer() et met à jour
	 * InOutLastEventTime (secondes, avant mise à l'échelle). Retourne vrai si l'attaque est gérée ici (les impacts
	 * génériques de HitTimes ne sont alors PAS programmés). Appelé après le montage et le télégraphe.
	 */
	virtual bool ScheduleCustomEvents(const FEMBBossAttackRow& Row, float TimeScale, float& InOutLastEventTime) { return false; }

	/** Un impact programmé (HitTimes) vient d'être résolu. Landed = joueurs touchés. Ex. Séisme : expose le noyau. */
	virtual void OnTimedHitExecuted(const FEMBBossAttackRow& Row, int32 HitIndex, const TArray<AActor*>& Landed) {}

	/** Attaque enchaînée automatiquement à la fin de Row (ex. Rugissement -> Poing-marteau). Tag invalide : aucune. */
	virtual FGameplayTag GetChainedAttackTag(const FEMBBossAttackRow& Row) const { return FGameplayTag(); }

	/** L'attaque programmée est annulée (étourdissement, transition, mort, nouvelle attaque). Nettoyer l'état propre. */
	virtual void OnAttackCancelled() {}

	/** Le boss vient de mourir au combat (avant la diffusion de l'événement). */
	virtual void OnDefeatedServer() {}

	/**
	 * Une attaque vient d'être SÉLECTIONNÉE (toutes familles : mêlée, souffle, piqué, charge...) et son démarrage
	 * est enregistré (recharge + historique). Appelé aussi pour une attaque enchaînée. Aucun effet par défaut.
	 * BeginTimedAttack est toujours précédé de cet appel : le hook est donc déclenché une seule fois par attaque.
	 */
	virtual void OnAttackStarted(const FEMBBossAttackRow& Row) {}

	/** La rencontre démarre (première détection / premier dégât) : l'échelle coop vient d'être figée. */
	virtual void OnEncounterBegun() {}

	/** La rencontre est abandonnée (aucun joueur vivant dans l'arène depuis WipeResetDelay) : remise à zéro du boss. */
	virtual void OnEncounterReset() {}

	/** Ajoute un timer lié à l'attaque en cours (annulé par CancelTimedAttack). */
	void AddAttackTimer(const FTimerDelegate& Delegate, float Delay);

	/**
	 * À appeler dans le constructeur de la classe fille : PV de base (solo), posture maximale, régénération de posture
	 * et délai avant régénération (propriétés de UEMBStatsComponent, lues avant le BeginPlay du composant).
	 */
	void ConfigureBossStats(float InMaxHealth, float InMaxPoise, float InPoiseRegenRate, float InPoiseRegenDelay);

	// --- Mise en scène (Blueprint) ---

	/** Mise en scène : appelé chez tous quand un télégraphe commence. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Boss", meta = (DisplayName = "On Attack Telegraph"))
	void BP_OnAttackTelegraph(FGameplayTag AttackTag, float Duration);

	/** Mise en scène : ombre / cercle d'impact au sol. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Boss", meta = (DisplayName = "On Show Ground Marker"))
	void BP_OnShowGroundMarker(FVector Location, float Radius, float Duration, bool bFlash);

	UFUNCTION()
	void OnRep_CurrentPhase(int32 OldPhase);

	UFUNCTION()
	void OnRep_IsTransitioning();

	UFUNCTION()
	void OnRep_IsStunned();

	UFUNCTION()
	void OnRep_IsDead();

	UFUNCTION()
	void HandleStatsDeath(AActor* Killer);

	UFUNCTION()
	void HandlePoiseBroken(AActor* InInstigator);

	// Requêtes réutilisables par les classes filles
	void GatherPlayerPawnsInSphere(const FVector& Center, float Radius, TArray<APawn*>& OutPawns, bool bHorizontalOnly = false) const;
	bool HasLineOfSight(const FVector& Origin, const AActor* Target) const;
	TArray<AActor*> ApplyHitToPawns(const TArray<APawn*>& Pawns, const FVector& Origin, const FEMBHitData& HitTemplate, bool bRequireLineOfSight, TSet<TWeakObjectPtr<AActor>>* InOutAlreadyHit = nullptr);

	// --- Configuration (constructeur / Blueprint fils) ---

	/** DataTable des attaques (lignes FEMBBossAttackRow). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	TObjectPtr<UDataTable> AttackTable;

	/** PV de base (solo) : posés par ConfigureBossStats ; multipliés selon le nombre de joueurs au début de la rencontre. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats", meta = (ClampMin = "1.0"))
	float BaseMaxHealth = 10000.f;

	/**
	 * Rayon de l'arène (cm) autour du boss : un joueur vivant à moins de ce rayon maintient la rencontre active
	 * (IsEncounterActive). Sans joueur vivant dans l'arène pendant WipeResetDelay (30 s), la rencontre est abandonnée.
	 * Dragon : 6 000 ; Colosse : 3 000 ; Nyxariel (4 000) à régler dans son constructeur.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats", meta = (ClampMin = "100.0"))
	float ArenaRadius = 5000.f;

	/** Bonus de PV par joueur supplémentaire : PV x (1 + bonus x (joueurs - 1)). Référentiel : 0,6. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats", meta = (ClampMin = "0.0"))
	float CoopHealthBonusPerExtraPlayer = 0.6f;

	/** Durée de l'étourdissement de posture brisée (s). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats", meta = (ClampMin = "0.1"))
	float StunDuration = EMBDragonDefaults::PostureBreakStunDuration;

	/** Multiplicateur de dégâts d'une riposte sur boss étourdi. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats", meta = (ClampMin = "1.0"))
	float CriticalHitMultiplier = EMBDragonDefaults::CriticalHitMultiplier;

	/**
	 * Seuils de PV (fraction 0..1, décroissants) déclenchant les phases 2, 3... Un seuil par transition :
	 * {0.70, 0.35} = P2 à 70 %, P3 à 35 %. Un tableau vide = boss à une seule phase.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases")
	TArray<float> PhaseHealthThresholds;

	/** Durée du rugissement / de l'invulnérabilité de transition (s). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases", meta = (ClampMin = "0.1"))
	float PhaseTransitionDuration = EMBDragonDefaults::PhaseTransitionDuration;

	/** Première phase "enragée" (0 = aucun enragé). Applique les trois multiplicateurs ci-dessous. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases", meta = (ClampMin = "0"))
	int32 EnrageFromPhase = 0;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases", meta = (ClampMin = "0.1"))
	float EnragedPlayRate = 1.f;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases", meta = (ClampMin = "0.1"))
	float EnragedCooldownMultiplier = 1.f;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Phases", meta = (ClampMin = "0.1"))
	float EnragedTelegraphMultiplier = 1.f;

	/**
	 * Résistance par type de dégâts : fraction retranchée (0,1 = -10 % ; -0,2 = +20 %, faiblesse). Type absent : 0.
	 * Appliquée aux dégâts de PV uniquement (pas à la posture).
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Stats")
	TMap<EEMBDamageType, float> DamageResistances;

	/** Multiplicateur de dégâts par os (permanent). Un os non listé remonte la hiérarchie du squelette jusqu'à un parent listé. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	TMap<FName, float> WeakPointMultipliers;

	/**
	 * Points faibles conditionnels : le multiplicateur de l'os n'est actif que pendant
	 * SetConditionalWeakPointsActive(true) ; sinon l'os compte pour x1,0. Colosse : "core" x1,5 exposé après le Séisme.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	TMap<FName, float> ConditionalWeakPointMultipliers;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	FName MouthSocketName = TEXT("Mouth");

	/** Dégâts subis pendant la fenêtre de vulnérabilité (1,0 : aucun bonus). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data", meta = (ClampMin = "1.0"))
	float VulnerableDamageMultiplier = 1.f;

	/** Canal de trace de la ligne de vue (Cataclysm). Visibility par défaut ; un canal "Cover" projet est plus robuste. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	TEnumAsByte<ECollisionChannel> LineOfSightChannel = ECC_Visibility;

	/** VFX d'une zone de feu persistante (traînée du souffle aérien). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Data")
	TSoftObjectPtr<UNiagaraSystem> FireZoneVFX;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Montages")
	TObjectPtr<UAnimMontage> PhaseTransitionMontage;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Montages")
	TObjectPtr<UAnimMontage> StunMontage;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Boss|Montages")
	TObjectPtr<UAnimMontage> DeathMontage;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Boss")
	TObjectPtr<UEMBStatsComponent> StatsComponent;

	FRandomStream RandomStream;

private:
	void CheckDefeatedFromSave();
	void TickWipeCheck();
	void ResetEncounter();
	void BuildAttackCache();
	void PreloadAttackAssets();
	void ScaleHealthForPlayerCount(int32 PlayerCount);

	void RefreshStateTags();
	void PushStateToBlackboard();
	float ResolveWeakPointMultiplier(FName BoneName) const;

	// Phases
	void EvaluatePhaseThresholds();
	void StartPhaseTransition(int32 TargetPhase);
	void FinishPhaseTransition();

	// Posture
	void EndStun();

	// Fin de vulnérabilité / de points faibles conditionnels
	void EndVulnerableWindow();
	void EndConditionalWeakPoints();

	// Impacts programmés
	void ExecuteMeleeHit(FGameplayTag AttackTag, int32 HitIndex);
	void StartChainedAttack(FGameplayTag AttackTag);
	void HandleEmberMarker(FGameplayTag AttackTag, float DelayToImpact);
	void ExecuteEmberImpact(FGameplayTag AttackTag, FVector Location);
	TArray<AActor*> ApplyRowShape(const FEMBBossAttackRow& Row);
	void ApplyKnockback(AActor* Target, float Distance);
	float EstimateTimedDuration(const FEMBBossAttackRow& Row) const;

	// Brûlure / zones de feu
	void TickBurn();
	void TickFireZones();

	// --- État répliqué ---
	UPROPERTY(ReplicatedUsing = OnRep_CurrentPhase)
	int32 CurrentPhase = 1;

	UPROPERTY(ReplicatedUsing = OnRep_IsTransitioning)
	bool bIsTransitioning = false;

	UPROPERTY(ReplicatedUsing = OnRep_IsStunned)
	bool bIsStunned = false;

	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;

	UPROPERTY(Replicated)
	bool bIsFlying = false;

	UPROPERTY(Replicated)
	bool bIsVulnerable = false;

	// --- État local ---
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> ActiveBreathVFX;

	/** Tags d'état (Boss.Phase.N, State.Combat.Invulnerable, State.Combat.Staggered, State.Dead), reconstruits localement. */
	FGameplayTagContainer StateTags;

	int32 PendingPhase = 1;
	bool bEncounterStarted = false;
	bool bFacingLocked = false;
	bool bConditionalWeakPointsActive = false;

	TArray<const FEMBBossAttackRow*> AttackRows;
	TMap<FGameplayTag, const FEMBBossAttackRow*> AttackRowByTag;
	TMap<FGameplayTag, double> CooldownEndTimes; // heure monde de fin de recharge
	FGameplayTag LastAttackTag;
	FGameplayTag PreviousAttackTag;

	TMap<TWeakObjectPtr<AActor>, FEMBBossThreatEntry> ThreatTable;
	TMap<TWeakObjectPtr<AActor>, double> BurnEndTimes;
	TArray<FEMBBossFireZone> FireZones;

	TArray<FTimerHandle> AttackTimers;
	FTimerHandle PhaseTransitionTimerHandle;
	FTimerHandle StunTimerHandle;
	FTimerHandle VulnerableTimerHandle;
	FTimerHandle WeakPointTimerHandle;
	FTimerHandle WipeCheckTimerHandle;
	double WipeStartTime = -1.0; // heure monde du début de l'absence de joueur dans l'arène (-1 : aucun wipe en cours)
	FTimerHandle BurnTimerHandle;
	FTimerHandle FireZoneTimerHandle;

	TSharedPtr<FStreamableHandle> PreloadHandle;
};
