// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBProgressionTypes.h"
#include "EMBSurvivalTypes.h"
#include "EMBSurvivalComponent.generated.h"

class UDataTable;
class UEMBCombatComponent;
class UEMBEnvironmentSubsystem;
class UEMBInventoryComponent;
class UEMBStatsComponent;

/** Jauges de survie rafraîchies (serveur à 1 Hz ; client à la réplication) : faim, soif, température ressentie, Corruption. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FEMBOnSurvivalChanged, float, Hunger, float, Thirst, float, FeltTemperature, float, Corruption);

/**
 * Survie du personnage : faim, soif, température ressentie, Corruption, consommables (contrat 10).
 *
 * ARCHITECTURE
 *  - Composant dédié sur le personnage (Dev A l'ajoute en sous-objet ; il fonctionne aussi s'il est ajouté
 *    par Blueprint : il retrouve lui-même les composants Stats, Combat et Inventaire de son propriétaire).
 *  - Toute la logique tourne sur le SERVEUR, à 1 Hz par timer (pas de Tick) ; les durées sont mesurées sur le
 *    temps monde, donc un timer retardé ne fausse pas les jauges.
 *  - Les effets sont POUSSÉS vers les stats (UEMBStatsComponent::SetSurvivalModifiers) et le combat
 *    (UEMBCombatComponent::SetExtraFireResistance) uniquement quand ils changent.
 *  - Les dégâts de survie passent par UEMBCombatComponent::ApplyHitToActor avec bDamageOverTime = true :
 *    dégâts directs (ni esquive, ni garde, ni parade, ni multicast), sans passer par OnHitReceived, donc
 *    sans retour de Corruption sur soi-même. Le routage garde le comportement d'un PNJ sans composant de
 *    combat (repli sur UEMBStatsComponent::ApplyDamage).
 *
 * RÉSEAU
 *  - Faim, soif, température, Corruption et états d'intoxication / huile : répliqués au PROPRIÉTAIRE seul
 *    (COND_OwnerOnly) : ce sont des données de HUD qui varient chaque seconde.
 *  - La Corruption est visible des autres joueurs à partir de 50 (effets visuels) : plutôt que de répliquer
 *    la valeur exacte (qui varierait chaque seconde), un PALIER (0, 1 = >= 50, 2 = >= 75) est répliqué aux
 *    non-propriétaires (COND_SkipOwner) et ne change que deux fois au plus par traversée de seuil.
 *  - Les clients demandent une consommation par RequestConsume -> Server_Consume (Reliable), validée côté serveur.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent), Config = Game)
class EMBERFALL_API UEMBSurvivalComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBSurvivalComponent();

	// ---------------------------------------------------------------- Lecture (contrat 10.4)

	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") float GetHunger() const { return Hunger; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") float GetThirst() const { return Thirst; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") float GetCorruption() const { return Corruption; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") float GetFeltTemperature() const { return FeltTemperature; }

	/** Corruption >= 25 : équivalent du tag State.Survival.Whispers (son d'ambiance). Propriétaire et serveur. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") bool HasWhispers() const;

	/** Vrai si le tag de survie donné est actif (State.Survival.Whispers). Propriétaire et serveur. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") bool HasSurvivalTag(FGameplayTag Tag) const;

	/**
	 * Palier de Corruption visible par tout le monde : 0 (< 50), 1 (>= 50), 2 (>= 75).
	 * Fonctionne pour le propriétaire, le serveur et les autres clients.
	 */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") uint8 GetVisibleCorruptionTier() const;

	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") bool IsIntoxicated() const { return bIntoxicated; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Survival") bool HasFireResistanceOil() const { return bFireOilActive; }

	// ---------------------------------------------------------------- Actions

	/** Client ou serveur : demande la consommation d'1 unité de la ressource (ligne de DT_Consumables). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void RequestConsume(FName ConsumableId);

	/** Serveur. Ajoute de la Corruption (multipliée par le passif CorruptionGain). Montant négatif ou non fini ignoré. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void AddCorruption(float Amount);

	/** Serveur. Retire de la Corruption (aucun passif appliqué). Montant négatif ou non fini ignoré. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void ReduceCorruption(float Amount);

	/** Serveur : valeurs de réapparition (9.2) : faim 60, soif 60, Corruption 0, température neutre, états temporaires effacés. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void ApplyRespawnValues();

	/** Serveur : réanimation par un allié. Corruption plafonnée à 60, intoxication purgée, autres jauges inchangées. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void ApplyReviveValues();

	/**
	 * Serveur. Passifs poussés par la progression : HungerThirstRate, ComfortRange, CorruptionGain.
	 * Le passif FireResistance n'est PAS relayé ici : la progression le pousse déjà au combat ; ce composant
	 * ne gère que le bonus temporaire de l'huile ignifuge (SetExtraFireResistance).
	 */
	void SetPassiveModifiers(const FEMBPassiveModifiers& Mods);

	/** Serveur. Isolation thermique de l'équipement en °C (0 en v1), bornée à [-40, 40]. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Survival")
	void SetInsulation(float DegreesC);

	void ExportSaveData(FEMBSurvivalSaveData& Out) const;

	/** Serveur. Import BORNÉ : valeurs non finies remplacées par les valeurs de départ, jauges dans [0, 100], Corruption <= 99. */
	void ImportSaveData(const FEMBSurvivalSaveData& In);

	/** Rafraîchissement de l'interface. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Survival")
	FEMBOnSurvivalChanged OnSurvivalChanged;

	// ---------------------------------------------------------------- Données

	/** DataTable des consommables (lignes FEMBConsumableRow, nom de ligne = id de ressource). Chargée sur le serveur au BeginPlay.
	 *  Configurable : DefaultGame.ini, section [/Script/Emberfall.EMBSurvivalComponent], clé ConsumablesTable=/Game/Data/DT_Consumables.DT_Consumables (défaut identique en code). */
	// Config sans EditDefaultsOnly : une surcharge de classe Blueprint serait écrasée par la config au chargement.
	UPROPERTY(Config, BlueprintReadOnly, Category = "Survival|Data")
	TSoftObjectPtr<UDataTable> ConsumablesTable;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	/** Effets de survie poussés vers les stats et le combat. */
	struct FEffects
	{
		float MaxHealthMultiplier = 1.f;
		float MaxStaminaMultiplier = 1.f;
		float StaminaRegenMultiplier = 1.f;
		float ExtraFireResistance = 0.f;
	};

	// ---- RPC
	UFUNCTION(Server, Reliable)
	void Server_Consume(FName ConsumableId);

	UFUNCTION() void OnRep_Gauges();

	/** Abonné à UEMBCombatComponent::OnHitReceived (serveur) : Corruption des coups reçus. */
	UFUNCTION() void HandleHitReceived(const FEMBHitData& Hit, EEMBHitResult Result);

	// ---- Boucle serveur
	void SurvivalTick();
	void ResolveDependencies();
	bool HasAuthorityOwner() const;
	UEMBEnvironmentSubsystem* GetEnvironment() const;

	/** Seuils de température décalés par ComfortRange : froid et chaleur, vers l'extérieur. */
	float GetComfortRange() const;
	bool IsColdForHunger() const;
	bool IsHotForThirst() const;

	/** Applique les dégâts de survie du tick (une seule frappe regroupée) ; retourne true si le joueur est mort. */
	bool ApplySurvivalDamage(float Amount);
	void KillByCorruption();

	FEffects ComputeEffects() const;
	void PushEffects(bool bForce);

	/** Après tout changement de Corruption : effets, mort à 100, état répliqué. */
	void OnCorruptionChanged();

	/** Met à jour les états répliqués dérivés (palier public, booléens) puis prévient l'interface. */
	void RefreshReplicatedState();
	void BroadcastSurvival();

	// ---- Consommables
	void ProcessConsume(FName ConsumableId);
	const FEMBConsumableRow* FindConsumableRow(FName ConsumableId) const;
	bool WouldHaveEffect(const FEMBConsumableRow& Row) const;
	void ApplyConsumable(const FEMBConsumableRow& Row);

	// ---- Jauges (répliquées au propriétaire)
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) float Hunger = EMBSurvivalDefaults::StartHunger;
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) float Thirst = EMBSurvivalDefaults::StartThirst;
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) float FeltTemperature = EMBSurvivalDefaults::NeutralTemperatureC;
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) float Corruption = 0.f;
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) bool bIntoxicated = false;
	UPROPERTY(ReplicatedUsing = OnRep_Gauges) bool bFireOilActive = false;

	/** Palier de Corruption pour les autres joueurs (0, 1 >= 50, 2 >= 75). Répliqué sauf au propriétaire. */
	UPROPERTY(Replicated) uint8 PublicCorruptionTier = 0;

	// ---- État serveur uniquement
	FEMBPassiveModifiers PassiveModifiers;
	float InsulationC = 0.f;

	float IntoxicationRemaining = 0.f;
	float IntoxicationDamagePerSecond = EMBSurvivalDefaults::DefaultIntoxicationDamagePerSecond;
	float FireOilRemaining = 0.f;
	float FireOilBonus = EMBSurvivalDefaults::DefaultFireOilBonus;

	FEffects AppliedEffects;
	bool bStatsEffectsApplied = false;
	bool bFireEffectApplied = false;

	double LastTickTime = 0.0;
	double LastConsumeTime = -1000.0;
	double ConsumeWindowStart = -1000.0;
	int32 ConsumeRequestsInWindow = 0;
	/** Côté client : dernier envoi de Server_Consume (évite d'inonder le réseau, le serveur reste l'arbitre). */
	double LastLocalConsumeRequestTime = -1000.0;

	FTimerHandle SurvivalTimerHandle;

	UPROPERTY(Transient) TObjectPtr<UEMBStatsComponent> Stats;
	UPROPERTY(Transient) TObjectPtr<UEMBCombatComponent> Combat;
	UPROPERTY(Transient) TObjectPtr<UEMBInventoryComponent> Inventory;
	UPROPERTY(Transient) TObjectPtr<UDataTable> LoadedConsumables;
};
