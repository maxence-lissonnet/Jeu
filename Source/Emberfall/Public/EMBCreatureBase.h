// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayTagContainer.h"
#include "Engine/StreamableManager.h"
#include "EMBCombatTypes.h"
#include "EMBCreatureTypes.h"
#include "EMBHitReceiver.h"
#include "EMBCreatureBase.generated.h"

class AEMBCreatureBase;
class AEMBCreatureSpawner;
class APlayerState;
class UAnimMontage;
class UDataTable;
class UEMBStatsComponent;

/** Mort de la créature (serveur uniquement). Killer = dernier instigateur du coup fatal. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnCreatureDied, AEMBCreatureBase*, Creature, AActor*, Killer);

/**
 * Créature légère d'Emberfall (référentiel §16) : goule de cendre, wyverneau des rocs. N'hérite PAS de AEMBBossBase :
 * ni phases, ni arène, ni mise à l'échelle coop. Une seule classe paramétrée par une ligne de DT_Creatures
 * (CreatureRowName) ; AEMBGhoulCreature et AEMBRockWyvernCreature (en bas de ce fichier) ne fixent que la ligne.
 *
 * Fonctions :
 *  - stats (UEMBStatsComponent) lues dans la ligne avant l'initialisation du composant ;
 *  - IEMBHitReceiver : dégâts, poise brisée -> étourdissement 0,8 s (Staggered), mémoire des contributeurs (30 s) ;
 *  - attaques chronométrées sur la table d'attaques de la ligne (format FEMBDragonAttackRow) : télégraphe, impacts
 *    programmés par timers, cône ou sphère, résolution par UEMBCombatComponent::ApplyHitToActor ;
 *  - mort : XP aux contributeurs (AddXP, <= 10 000 cm), carcasse (SpawnDynamicNode, 600 s), ragdoll ou montage,
 *    disparition, signalement au spawner ;
 *  - enregistrement dans UEMBCreatureSignificance (budget serveur de 40 et paliers de détail).
 *
 * Architecture réseau : serveur autoritaire. Réplication minimale : mouvement standard (ACharacter), jauges (stats),
 * bIsDead (RepNotify). Les montages et les télégraphes sont des NetMulticast. Aucun Tick d'acteur : timers et BT.
 * Les clients lisent la ligne de données (animation, surélévation du vol) mais n'exécutent aucune logique de jeu.
 *
 * Le contrôleur par défaut est AEMBCreatureAIController (arbre BT_Creature décrit dans EMBCreatureAIController.h).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBCreatureBase : public ACharacter, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBCreatureBase();

	// --- AActor / ACharacter ---
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreInitializeComponents() override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// --- IEMBHitReceiver ---
	/** Serveur uniquement. Dégâts, poise, mémoire des contributeurs, aggro. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	// ------------------------------------------------------------------------------------------
	// Données
	// ------------------------------------------------------------------------------------------

	/** Lit la ligne de données de la classe (CDO) : sert au spawner (taille de groupe, poids de nuit). */
	static bool FindRowForClass(const UClass* CreatureClass, FEMBCreatureRow& OutRow);

	bool HasCreatureRow() const { return bHasRow; }
	const FEMBCreatureRow& GetCreatureRow() const { return CachedRow; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Creature")
	UEMBStatsComponent* GetStatsComponent() const { return StatsComponent; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Creature")
	bool IsDead() const { return bIsDead; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Creature")
	bool IsStaggered() const { return bIsStaggered; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Creature")
	bool IsAttacking() const { return bAttacking; }

	/** Point d'apparition : centre de la patrouille et de la laisse. */
	FVector GetHomeLocation() const { return HomeLocation; }

	// ------------------------------------------------------------------------------------------
	// Spawner
	// ------------------------------------------------------------------------------------------

	/** Appelé par le spawner avant FinishSpawning. */
	void SetOwningSpawner(AEMBCreatureSpawner* Spawner);

	/** Vrai si un spawner a créé cette créature (sinon elle n'est jamais rendue : elle a été posée à la main). */
	bool HasOwningSpawner() const { return OwningSpawner.IsValid(); }

	/** Serveur : la créature n'est plus pertinente. Elle est détruite et son emplacement redevient disponible. */
	void ReturnToSpawner();

	// ------------------------------------------------------------------------------------------
	// Interface pour l'IA (serveur)
	// ------------------------------------------------------------------------------------------

	/** Vrai si PV/PVmax <= FleeHealthFraction de la ligne (0 : jamais). */
	bool IsBelowFleeThreshold() const;

	/** Vitesse de patrouille (false) ou de course (true) : poursuite, fuite, retour. */
	void SetRunning(bool bRunning);

	/** Au moins une attaque non en recharge est utilisable contre la cible (portée et angle). */
	bool HasAttackInRange(const AActor* Target) const;

	/**
	 * Choisit (tirage pondéré anti-répétition) et lance une attaque contre la cible. Retourne la durée totale à
	 * attendre (impacts + récupération), 0 si aucune attaque n'est utilisable ou si la créature ne peut pas attaquer.
	 */
	float StartAttackOn(AActor* Target);

	/** Annule l'attaque en cours (impacts programmés, orientation figée). Idempotent. */
	void CancelAttack(bool bStopMontage);

	/** Dernier joueur ayant frappé la créature, s'il l'a fait il y a moins de MaxAge secondes (sinon nullptr). */
	APawn* GetRecentAttacker(float MaxAge) const;

	const FEMBCreatureAttackRow* FindAttackRow(const FGameplayTag& AttackTag) const;

	/** Diffusé sur le serveur à la mort (avant la disparition du cadavre). */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Creature")
	FEMBOnCreatureDied OnCreatureDied;

	// ------------------------------------------------------------------------------------------
	// Réseau : effets cosmétiques
	// ------------------------------------------------------------------------------------------

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_PlayMontage(UAnimMontage* Montage, float PlayRate);

	/** Montage d'attaque : Unreliable (ponctuel, cosmétique). Mort et étourdissement restent en Multicast_PlayMontage (Reliable). */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayAttackMontage(UAnimMontage* Montage, float PlayRate);

	UFUNCTION(NetMulticast, Reliable)
	void Multicast_StopMontages(float BlendOutTime);

	/** Télégraphe visuel et sonore (griffe levée, gueule ouverte...) : appelle l'événement Blueprint chez tous. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_Telegraph(FGameplayTag AttackTag, float Duration);

protected:
	/** Mise en scène : un télégraphe commence (chez tous). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Creature", meta = (DisplayName = "On Attack Telegraph"))
	void BP_OnAttackTelegraph(FGameplayTag AttackTag, float Duration);

	UFUNCTION()
	void OnRep_IsDead();

	UFUNCTION()
	void HandleStatsDeath(AActor* Killer);

	UFUNCTION()
	void HandlePoiseBroken(AActor* InInstigator);

	// --- Configuration (Blueprint de la créature) ---

	/** Table des créatures (lignes FEMBCreatureRow). Par défaut /Game/Data/DT_Creatures. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Data")
	TSoftObjectPtr<UDataTable> CreatureTable;

	/** Ligne de la table décrivant cette créature (fixée par classe : le client lit la même ligne que le serveur). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Data")
	FName CreatureRowName;

	/** Montage de mort. S'il est absent, la créature passe en ragdoll (hors serveur dédié). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Montages")
	TObjectPtr<UAnimMontage> DeathMontage;

	/** Montage d'étourdissement (poise brisée). Optionnel. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Montages")
	TObjectPtr<UAnimMontage> StaggerMontage;

	/** Surélévation du mesh d'une créature volante (vol bas). La capsule reste au sol (navigation, collisions). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Data", meta = (ClampMin = "0.0"))
	float HoverHeight = EMBCreatureDefaults::HoverHeight;

	/** Délai (s) entre la mort et la disparition du cadavre. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Creature|Data", meta = (ClampMin = "0.1"))
	float DeathLifeSpan = EMBCreatureDefaults::DeathLifeSpan;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Creature")
	TObjectPtr<UEMBStatsComponent> StatsComponent;

	FRandomStream RandomStream;

private:
	// Données
	bool LoadRow(FEMBCreatureRow& OutRow) const;
	void BuildAttackCache();
	void PreloadAttackAssets();
	UAnimMontage* ResolveMontage(const TSoftObjectPtr<UAnimMontage>& Soft) const;

	// Sélection et exécution des attaques
	float ComputeAttackWeight(const FEMBCreatureAttackRow& Row, float Distance, float AngleDeg) const;
	const FEMBCreatureAttackRow* SelectAttack(const AActor* Target);
	void RegisterAttackSelected(const FEMBCreatureAttackRow& Row);
	bool IsAttackOnCooldown(const FGameplayTag& AttackTag) const;
	float ExecuteAttack(const FEMBCreatureAttackRow& Row, AActor* Target);
	void AddAttackTimer(const FTimerDelegate& Delegate, float Delay);
	void ExecuteHit(FGameplayTag AttackTag, int32 HitIndex);
	void ExecuteLunge(FGameplayTag AttackTag, TWeakObjectPtr<AActor> TargetPtr);
	void LockFacing();
	void FinishAttack();
	FEMBHitData MakeHitData(const FEMBCreatureAttackRow& Row);
	void SetFacingLocked(bool bLocked);

	// Étourdissement
	void BeginStagger(float Duration);
	void EndStagger();

	// Mort
	void RegisterContributor(AActor* Instigator);
	void AwardExperience();
	void SpawnCarcass();
	void ApplyDeathPresentation();
	void PushStateToBlackboard();

	// --- État répliqué ---
	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;

	// --- État local ---
	FEMBCreatureRow CachedRow;
	bool bHasRow = false;

	/** Table d'attaques de la ligne (référence forte : les pointeurs de lignes restent valides). Serveur seulement. */
	UPROPERTY(Transient)
	TObjectPtr<UDataTable> AttackTableAsset;

	TArray<const FEMBCreatureAttackRow*> AttackRows;
	TMap<FGameplayTag, const FEMBCreatureAttackRow*> AttackRowByTag;
	TMap<FGameplayTag, double> CooldownEndTimes; // heure monde de fin de recharge
	FGameplayTag LastAttackTag;

	TArray<FTimerHandle> AttackTimers;
	FTimerHandle StaggerTimerHandle;

	/** Joueurs ayant infligé des dégâts -> heure monde du dernier coup (XP à la mort). */
	TMap<TWeakObjectPtr<APlayerState>, double> Contributors;

	TWeakObjectPtr<APawn> LastAttacker;
	double LastAttackerTime = -1000.0;

	TWeakObjectPtr<AEMBCreatureSpawner> OwningSpawner;
	FVector HomeLocation = FVector::ZeroVector;
	bool bAttacking = false;
	bool bIsStaggered = false;

	TSharedPtr<FStreamableHandle> PreloadHandle;
};

/** Goule de cendre : la ligne "Ghoul" de DT_Creatures. Rien d'autre à coder, tout est dans les données. */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBGhoulCreature : public AEMBCreatureBase
{
	GENERATED_BODY()

public:
	AEMBGhoulCreature();
};

/** Wyverneau des rocs : la ligne "RockWyvern" de DT_Creatures (vol bas, fuit à 30 % de PV). */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBRockWyvernCreature : public AEMBCreatureBase
{
	GENERATED_BODY()

public:
	AEMBRockWyvernCreature();
};
