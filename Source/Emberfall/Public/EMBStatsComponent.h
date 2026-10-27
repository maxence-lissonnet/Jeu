// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMBStatsComponent.generated.h"

UENUM(BlueprintType)
enum class EEMBStatType : uint8
{
	Health,
	Stamina,
	Poise
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FEMBOnAttributeChanged, EEMBStatType, StatType, float, NewValue, float, MaxValue);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnDeath, AActor*, Killer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnPoiseBroken, AActor*, Instigator);

/**
 * Jauges vitales (vie, endurance, poise) d'un acteur : joueur ET boss (la « posture » d'un boss est sa poise).
 *
 * Réseau : le serveur est seul autorisé à modifier les jauges (ApplyDamage, ConsumeStamina...).
 * Les valeurs sont répliquées ; les RepNotify rediffusent OnAttributeChanged côté client pour l'UI.
 * Les délégués OnDeath et OnPoiseBroken ne sont diffusés que sur le serveur (la logique de jeu y vit) ;
 * un client qui veut réagir à la mort écoute OnAttributeChanged (Health == 0) ou l'état de combat répliqué.
 */
UCLASS(ClassGroup=(Emberfall), meta=(BlueprintSpawnableComponent))
class EMBERFALL_API UEMBStatsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBStatsComponent();

	// ---------------------------------------------------------------- Contrat (section 3.1)

	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetHealth() const { return Health; }
	/** Maximum EFFECTIF : valeur de base x multiplicateur de survie (Corruption...). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetMaxHealth() const;
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetStamina() const { return Stamina; }
	/** Maximum EFFECTIF : valeur de base x multiplicateur de survie (soif...). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetMaxStamina() const;
	/** Serveur : valeur exacte. Client : valeur répliquée, mise à jour par pas d'au moins 1 point (économie de bande passante). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetPoise() const;
	/** Maximum EFFECTIF : valeur de base + bonus d'équipement (armures). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetMaxPoise() const { return MaxPoise + EquipmentPoiseBonus; }

	/** Vie restante entre 0 et 1. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") float GetHealthPercent() const;

	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") bool IsDead() const { return Health <= 0.f; }

	/** Règle « souls » : une action est permise tant que la jauge n'est pas vide (elle peut passer à 0). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Stats") bool CanAffordStamina() const { return Stamina > 0.f; }

	/**
	 * Serveur uniquement. Retourne false si l'endurance est déjà <= 0 ; sinon soustrait (clamp à 0)
	 * et relance le délai de régénération (0,8 s ; 1,5 s si la jauge tombe à 0).
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") bool ConsumeStamina(float Amount);

	/** Serveur. Retourne les dégâts effectivement retirés ; diffuse OnDeath si la vie atteint 0. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") float ApplyDamage(float Amount, AActor* DamageCauser);

	/** Serveur. Retourne true si la poise est brisée (elle est alors remise au maximum, puis OnPoiseBroken). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") bool ApplyPoiseDamage(float Amount, AActor* Instigator);

	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void Heal(float Amount);

	/** 0,35 pendant la garde, 1,0 sinon. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void SetStaminaRegenMultiplier(float Multiplier);

	/** Mise à l'échelle coop des boss. Si bRefill est faux, la vie courante est simplement plafonnée au nouveau maximum. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void SetMaxHealth(float NewMax, bool bRefill);

	/**
	 * Serveur, poussé par la survie. Multiplicateurs appliqués aux maxima effectifs de vie et d'endurance
	 * (les valeurs courantes sont plafonnées au nouveau maximum, sans soin) et à la régénération d'endurance,
	 * en plus du multiplicateur de garde. Neutres (1, 1, 1) = comportement de base.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void SetSurvivalModifiers(float MaxHealthMultiplier, float MaxStaminaMultiplier, float StaminaRegenMultiplierFromSurvival);

	/**
	 * Serveur. Fait sortir de l'état mort : vie et endurance à la fraction demandée de leur maximum effectif
	 * (la vie reste >= 1), poise pleine. Sans effet si l'acteur n'est pas mort.
	 */
	/**
	 * Serveur, poussé par l'équipement. Bonus de poise des armures, ajouté à MaxPoise. La poise courante est
	 * plafonnée au nouveau maximum (jamais relevée : pas de poise gratuite en changeant d'armure).
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void SetEquipmentPoiseBonus(float Bonus);

	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void ReviveWithFraction(float HealthFraction, float StaminaFraction);

	/**
	 * Serveur. Restaure vie et endurance depuis une sauvegarde (valeurs bornées : vie entre 1 et le maximum,
	 * endurance entre 0 et le maximum ; la poise repart pleine). Rediffuse les jauges.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Stats") void RestoreFromSave(float InHealth, float InStamina);

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Stats") FEMBOnAttributeChanged OnAttributeChanged;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Stats") FEMBOnDeath OnDeath;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Stats") FEMBOnPoiseBroken OnPoiseBroken;

	// ---------------------------------------------------------------- Valeurs de conception (surchargeables : boss)
	// Valeurs par défaut = section 4.1 du référentiel (EMBCombatDefaults).

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing = OnRep_MaxHealth, BlueprintReadOnly, Category = "Stats|Vie", meta = (ClampMin = "1"))
	float MaxHealth;

	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Stats|Endurance", meta = (ClampMin = "1"))
	float MaxStamina;

	/** Régénération d'endurance par seconde. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats|Endurance", meta = (ClampMin = "0"))
	float StaminaRegenRate;

	/** Délai avant la reprise de la régénération après une consommation. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats|Endurance", meta = (ClampMin = "0"))
	float StaminaRegenDelay;

	/** Délai de régénération lorsque la jauge est tombée à 0 (épuisement). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats|Endurance", meta = (ClampMin = "0"))
	float ExhaustedRegenDelay;

	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Stats|Poise", meta = (ClampMin = "1"))
	float MaxPoise;

	/** Régénération de poise par seconde (boss : posture, 40/s dans le référentiel). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats|Poise", meta = (ClampMin = "0"))
	float PoiseRegenRate;

	/** Délai sans dégât de poise avant la régénération (joueur : 2 s ; boss : 4 s). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stats|Poise", meta = (ClampMin = "0"))
	float PoiseRegenDelay;

protected:
	virtual void InitializeComponent() override;
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION() void OnRep_Health(float OldValue);
	UFUNCTION() void OnRep_Stamina(float OldValue);
	UFUNCTION() void OnRep_Poise(float OldValue);
	UFUNCTION() void OnRep_MaxHealth(float OldValue);
	UFUNCTION() void OnRep_MaxMultipliers();

	bool HasAuthorityOwner() const;
	void BroadcastAttribute(EEMBStatType Type) const;

	/** Réveille le Tick de régénération (serveur) lorsqu'une jauge vient de quitter son maximum. */
	void WakeRegeneration();

	UPROPERTY(ReplicatedUsing = OnRep_Health) float Health;

	/** Endurance : utile surtout au propriétaire (HUD) ; répliquée uniquement à lui, voir GetLifetimeReplicatedProps. */
	UPROPERTY(ReplicatedUsing = OnRep_Stamina) float Stamina;

	/** Poise exacte (autorité serveur), non répliquée directement. */
	float Poise;

	/**
	 * Copie répliquée de la poise, rafraîchie seulement si elle a varié d'au moins 1 point (ou touche 0 / le
	 * maximum). La poise régénère en continu (15/s, 40/s pour un boss) : sans ce seuil, chaque tick de
	 * régénération serait un envoi réseau. Répliquée à tous (l'UI de posture du boss en a besoin).
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Poise) float RepPoise;

	void SyncReplicatedPoise();

	/** Multiplicateur de régénération d'endurance (serveur uniquement, non répliqué). */
	float StaminaRegenMultiplier = 1.f;

	/** Multiplicateur de régénération venant de la survie (faim, chaleur) ; combiné à celui de la garde. Serveur seulement. */
	float SurvivalStaminaRegenMultiplier = 1.f;

	/** Multiplicateurs de maxima venant de la survie. Vie : à tous (effets visuels) ; endurance : propriétaire. */
	UPROPERTY(ReplicatedUsing = OnRep_MaxMultipliers) float HealthMaxMultiplier = 1.f;
	UPROPERTY(ReplicatedUsing = OnRep_MaxMultipliers) float StaminaMaxMultiplier = 1.f;

	/** Bonus de poise maximale venant de l'équipement. Répliqué : l'interface de posture lit le maximum effectif. */
	UPROPERTY(ReplicatedUsing = OnRep_MaxMultipliers) float EquipmentPoiseBonus = 0.f;

	/** Instants (temps monde serveur) de reprise de la régénération. */
	double StaminaRegenResumeTime = 0.0;
	double PoiseRegenResumeTime = 0.0;
};
