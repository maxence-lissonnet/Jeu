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
#include "EMBDragonBoss.generated.h"

class UAnimMontage;
class UDataTable;
class UEMBStatsComponent;
class UNiagaraComponent;
class UNiagaraSystem;

/** Changement de phase effectif (fin du rugissement). Diffusé sur le serveur ET sur les clients (RepNotify). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnDragonPhaseChanged, int32, NewPhase, int32, OldPhase);

/** Début d'une transition de phase (rugissement, invulnérabilité). PendingPhase = phase qui va suivre. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnDragonPhaseTransitionStarted, int32, PendingPhase);

/** Posture brisée / fin d'étourdissement. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnDragonStunChanged, bool, bStunned);

/** Mort du boss : EventTag = Event.Boss.Killed.Dragon. Serveur uniquement (la progression est autoritaire). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnBossDefeated, FGameplayTag, EventTag, AActor*, Killer);

/** Fin de la rencontre : boss vaincu en combat, ou marqué vaincu par une sauvegarde. Serveur et clients (RepNotify). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMBOnDragonEncounterEnded);

/** Zone de feu persistante (souffle aérien). Données serveur uniquement, jamais répliquées. */
struct FEMBDragonFireZone
{
	FVector Center = FVector::ZeroVector;
	float Radius = 0.f;
	double EndTime = 0.0;
	float DamagePerSecond = 0.f;
};

/** Menace accumulée par un joueur (dégâts infligés au boss, avec décroissance exponentielle). */
struct FEMBDragonThreatEntry
{
	float Value = 0.f;
	double LastUpdateTime = 0.0;
};

/**
 * Vaelthyrax, l'Ancêtre-Braise : boss final (dragon ancestral, 3 phases).
 *
 * Architecture réseau : serveur autoritaire. Toute la logique (phases, dégâts, recharges, souffles) ne
 * s'exécute que si HasAuthority(). Les clients reçoivent :
 *  - les PV / la posture via UEMBStatsComponent (répliqué) ;
 *  - la phase (RepNotify), l'étourdissement, la transition, le vol et la mort via des propriétés répliquées ;
 *  - les montages et les VFX via des NetMulticast (cosmétiques, ignorés sur serveur dédié quand c'est possible).
 * Aucun Tick : tout est piloté par des événements, des timers et les tâches du Behavior Tree.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBDragonBoss : public ACharacter, public IEMBHitReceiver, public IGameplayTagAssetInterface
{
	GENERATED_BODY()

public:
	AEMBDragonBoss();

	// --- AActor / ACharacter ---
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// --- IEMBHitReceiver ---
	/** Serveur uniquement. Points faibles par os, invulnérabilité de transition, critique sur boss étourdi. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	// --- IGameplayTagAssetInterface ---
	virtual void GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const override;

	// ------------------------------------------------------------------------------------------
	// Délégués
	// ------------------------------------------------------------------------------------------

	/** Abonnement de l'arène (piliers, ponts, lave). Déclenché sur serveur et clients. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Dragon")
	FEMBOnDragonPhaseChanged OnPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Dragon")
	FEMBOnDragonPhaseTransitionStarted OnPhaseTransitionStarted;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Dragon")
	FEMBOnDragonStunChanged OnStunChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Dragon")
	FEMBOnBossDefeated OnBossDefeated;

	/** Rencontre terminée (mort au combat OU boss déjà vaincu d'après la sauvegarde) : l'arène passe en état final calme. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Dragon")
	FEMBOnDragonEncounterEnded OnEncounterEnded;

	/**
	 * Marque le dragon comme déjà vaincu (monde chargé d'une sauvegarde où Event.Boss.Killed.Dragon est enregistré).
	 * Serveur uniquement, idempotent. Le dragon reste en place mais dormant (caché, sans collision, sans IA) ;
	 * aucun XP n'est redonné et OnBossDefeated n'est PAS rediffusé. Appelable par le système de sauvegarde
	 * (restauration asynchrone possiblement postérieure au BeginPlay) ; le BeginPlay fait aussi le contrôle.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Dragon")
	void ApplyDefeatedFromSave();

	/**
	 * Vrai tant qu'une rencontre est en cours : engagée, boss vivant ET au moins un joueur vivant à moins de
	 * ParticipantRadius (UEMBProgressionSettings) du boss (après un wipe, la rencontre n'est plus active : la sauvegarde n'est pas bloquée).
	 * Le boss n'est pas réinitialisé après un wipe (hors périmètre).
	 */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsEncounterActive() const;

	// ------------------------------------------------------------------------------------------
	// État (lecture)
	// ------------------------------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	int32 GetCurrentPhase() const { return CurrentPhase; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsFlying() const { return bIsFlying; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsStunned() const { return bIsStunned; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsTransitioning() const { return bIsTransitioning; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsVulnerable() const { return bIsVulnerable; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsDead() const { return bIsDead; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	bool IsEnraged() const { return CurrentPhase >= 3; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	UEMBStatsComponent* GetStatsComponent() const { return StatsComponent; }

	/** Vitesse des montages : 1,0 ; 1,15 en phase 3 (enragé). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	float GetAttackPlayRate() const;

	/** Multiplicateur des télégraphes codés (souffles, piqué) : 1,0 ; 0,9 en phase 3. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	float GetTelegraphMultiplier() const;

	/** Multiplicateur des recharges : 1,0 ; 0,8 en phase 3. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Dragon")
	float GetCooldownMultiplier() const;

	/** Tag Boss.Phase.N correspondant. */
	static FGameplayTag GetPhaseTag(int32 Phase);

	// ------------------------------------------------------------------------------------------
	// Données d'attaque, recharges et historique (serveur)
	// ------------------------------------------------------------------------------------------

	const FEMBDragonAttackRow* FindAttackRow(const FGameplayTag& AttackTag) const;
	const TArray<const FEMBDragonAttackRow*>& GetAttackRows() const { return AttackRows; }

	bool IsAttackOnCooldown(const FGameplayTag& AttackTag) const;
	float GetCooldownRemaining(const FGameplayTag& AttackTag) const;

	/** Enregistre l'attaque choisie : historique des 2 dernières attaques + démarrage de la recharge. */
	void RegisterAttackSelected(const FEMBDragonAttackRow& Row);

	const FGameplayTag& GetLastAttackTag() const { return LastAttackTag; }
	const FGameplayTag& GetPreviousAttackTag() const { return PreviousAttackTag; }

	// ------------------------------------------------------------------------------------------
	// Exécution des attaques
	// ------------------------------------------------------------------------------------------

	/**
	 * Serveur : joue un montage chez tous (NetMulticast) et retourne sa durée réelle en secondes
	 * (longueur / vitesse). PlayRate <= 0 : vitesse de phase. Retourne 0 si montage nul ou client.
	 */
	float PlayAttackMontage(UAnimMontage* Montage, float PlayRate = -1.f, FName StartSection = NAME_None);

	/**
	 * Attaques non latentes (mêlée, saisie, rafale d'ailes, pluie de braises) : joue le montage, programme les
	 * impacts par timers et retourne la durée totale à attendre (écrite dans le Blackboard par SelectAttack).
	 */
	float BeginTimedAttack(const FEMBDragonAttackRow& Row);

	/** Annule les impacts programmés (étourdissement, transition, mort, abandon du BT). */
	void CancelTimedAttack(bool bStopMontage);

	/** Passe en vol (MOVE_Flying) ou revient au sol (MOVE_Falling puis atterrissage automatique). Serveur. */
	void SetFlying(bool bNewFlying);

	/**
	 * Déplace le dragon vers Destination à vitesse constante (vol scripté : décollage, piqué, strafe).
	 * Retourne vrai si arrivé ; bOutBlocked vrai si le balayage a heurté la géométrie.
	 */
	bool StepFlightToward(const FVector& Destination, float Speed, float DeltaSeconds, bool& bOutBlocked);

	/** Fenêtre de vulnérabilité au sol (après le piqué : 2,5 s). */
	void BeginVulnerableWindow(float Duration);

	/** Le dragon cesse (true) ou reprend (false) de suivre sa cible du regard : engagement d'une attaque. */
	void SetFacingLocked(bool bLocked);
	bool IsFacingLocked() const { return bFacingLocked; }

	/** Fait pivoter le dragon vers un cap (yaw, degrés) à vitesse angulaire constante. */
	void RotateYawToward(float TargetYawDeg, float DeltaSeconds, float DegreesPerSecond);

	/** Cataclysm scripté : entrée en phase 3 + 20 s, puis toutes les 75 s. */
	bool IsCataclysmDue() const;
	void NotifyCataclysmFired();

	// ------------------------------------------------------------------------------------------
	// Dégâts de zone (serveur)
	// ------------------------------------------------------------------------------------------

	/** Gabarit de coup rempli depuis une ligne de la DataTable (Instigator = ce boss). */
	FEMBHitData MakeHitData(const FEMBDragonAttackRow& Row);

	/**
	 * Touche les joueurs dans une sphère (requête d'overlap serveur, filtrage Pawn joueur).
	 * bRequireLineOfSight : un obstacle entre Center et le joueur annule le coup (Cataclysm).
	 * Retourne les joueurs effectivement touchés (ni esquivé, ni paré, ni ignoré).
	 */
	TArray<AActor*> ApplyAreaDamage(const FVector& Center, float Radius, const FEMBHitData& HitTemplate, bool bRequireLineOfSight = false);

	/** Idem pour un cône (demi-angle en degrés, longueur en cm) ; marge angulaire = rayon de capsule de la cible. */
	TArray<AActor*> ApplyConeDamage(const FVector& Origin, const FVector& Direction, float HalfAngleDeg, float Length, const FEMBHitData& HitTemplate, bool bRequireLineOfSight = false, bool bHorizontalOnly = false);

	/** Brûlure 15/s pendant 4 s, rafraîchie (non cumulée) à chaque nouvelle application. */
	void ApplyBurn(AActor* Target);

	/** Zone de feu persistante (souffle aérien : 8 s, 20/s). */
	void AddFireZone(const FVector& Center, float Radius, float Duration, float DamagePerSecond);

	/** Position de la gueule (socket MouthSocketName, sinon estimation devant le dragon). */
	FVector GetBreathOrigin() const;

	/** Projette un point sur le sol (trace verticale). Retourne le point d'entrée si rien n'est touché. */
	FVector ProjectToGround(const FVector& Location) const;

	// ------------------------------------------------------------------------------------------
	// Contexte de combat
	// ------------------------------------------------------------------------------------------

	/** Démarre le combat (mise à l'échelle coop des PV). Idempotent, appelé à la première détection d'un joueur. */
	void BeginEncounter();

	/** Distance à plat (cm) et angle signé (degrés, positif = cible à droite) du dragon vers l'acteur. */
	void GetDistanceAndAngleTo(const AActor* Target, float& OutDistance, float& OutSignedAngleDeg) const;

	int32 CountPlayersWithin(float Radius) const;

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

	/** Marqueur au sol (ombre du piqué, cercle de braises, foudre). bFlash : flash final avant l'impact. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_ShowGroundMarker(FVector Location, float Radius, float Duration, bool bFlash);

	/** VFX des zones de feu persistantes (souffle aérien). */
	UNiagaraSystem* ResolveFireZoneVFX() const { return ResolveVFX(FireZoneVFX); }

	UNiagaraSystem* ResolveVFX(const TSoftObjectPtr<UNiagaraSystem>& Soft) const;
	UAnimMontage* ResolveMontage(const TSoftObjectPtr<UAnimMontage>& Soft) const;

protected:
	/** Mise en scène : appelé chez tous quand un télégraphe commence. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Dragon", meta = (DisplayName = "On Attack Telegraph"))
	void BP_OnAttackTelegraph(FGameplayTag AttackTag, float Duration);

	/** Mise en scène : ombre / cercle d'impact au sol. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Dragon", meta = (DisplayName = "On Show Ground Marker"))
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

	// --- Configuration (Blueprint enfant) ---

	/** DataTable DT_DragonAttacks (lignes FEMBDragonAttackRow). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data")
	TObjectPtr<UDataTable> AttackTable;

	/** Multiplicateur de dégâts par os. Un os non listé remonte la hiérarchie du squelette jusqu'à un parent listé. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data")
	TMap<FName, float> WeakPointMultipliers;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data")
	FName MouthSocketName = TEXT("Mouth");

	/** Dégâts subis pendant la fenêtre de vulnérabilité du piqué (1,0 : aucun bonus, à équilibrer en playtest). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data", meta = (ClampMin = "1.0"))
	float VulnerableDamageMultiplier = EMBDragonDefaults::VulnerableDamageMultiplier;

	/** Canal de trace de la ligne de vue (Cataclysm). Visibility par défaut ; un canal "Cover" projet est plus robuste. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data")
	TEnumAsByte<ECollisionChannel> LineOfSightChannel = ECC_Visibility;

	/** VFX d'une zone de feu persistante (traînée du souffle aérien). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Data")
	TSoftObjectPtr<UNiagaraSystem> FireZoneVFX;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Montages")
	TObjectPtr<UAnimMontage> PhaseTransitionMontage;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Montages")
	TObjectPtr<UAnimMontage> StunMontage;

	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Dragon|Montages")
	TObjectPtr<UAnimMontage> DeathMontage;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Dragon")
	TObjectPtr<UEMBStatsComponent> StatsComponent;

private:
	void CheckDefeatedFromSave();
	void BuildAttackCache();
	void PreloadAttackAssets();
	void ConfigureStatsDefaults();
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

	// Fin de vulnérabilité
	void EndVulnerableWindow();

	// Impacts programmés
	void ExecuteMeleeHit(FGameplayTag AttackTag);
	void HandleEmberMarker(FGameplayTag AttackTag, float DelayToImpact);
	void ExecuteEmberImpact(FGameplayTag AttackTag, FVector Location);
	void ApplyRowShape(const FEMBDragonAttackRow& Row);
	void ApplyKnockback(AActor* Target, float Distance);

	// Brûlure / zones de feu
	void TickBurn();
	void TickFireZones();

	// Requêtes
	void GatherPlayerPawnsInSphere(const FVector& Center, float Radius, TArray<APawn*>& OutPawns) const;
	bool HasLineOfSight(const FVector& Origin, const AActor* Target) const;
	TArray<AActor*> ApplyHitToPawns(const TArray<APawn*>& Pawns, const FVector& Origin, const FEMBHitData& HitTemplate, bool bRequireLineOfSight);

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
	double NextCataclysmTime = -1.0;

	TArray<const FEMBDragonAttackRow*> AttackRows;
	TMap<FGameplayTag, const FEMBDragonAttackRow*> AttackRowByTag;
	TMap<FGameplayTag, double> CooldownEndTimes; // heure monde de fin de recharge
	FGameplayTag LastAttackTag;
	FGameplayTag PreviousAttackTag;

	TMap<TWeakObjectPtr<AActor>, FEMBDragonThreatEntry> ThreatTable;
	TMap<TWeakObjectPtr<AActor>, double> BurnEndTimes;
	TArray<FEMBDragonFireZone> FireZones;

	TArray<FTimerHandle> AttackTimers;
	FTimerHandle PhaseTransitionTimerHandle;
	FTimerHandle StunTimerHandle;
	FTimerHandle VulnerableTimerHandle;
	FTimerHandle BurnTimerHandle;
	FTimerHandle FireZoneTimerHandle;

	FRandomStream RandomStream;
	TSharedPtr<FStreamableHandle> PreloadHandle;
};
