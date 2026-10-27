// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EMBSurvivalTypes.generated.h"

/**
 * Constantes de la survie (référentiel 00_Constantes_et_Contrats.md, sections 9.2, 10.2 et 10.3).
 * Valeurs de conception à équilibrer en playtest : ce ne sont pas des mesures.
 */
namespace EMBSurvivalDefaults
{
	// ---- Cadence ----
	/** Période du timer serveur de la survie (1 Hz : les jauges sont lentes). */
	inline constexpr float TickInterval = 1.0f;
	/** Plafond du delta mesuré entre deux ticks (timer retardé par un hitch) : évite un saut de jauge. */
	inline constexpr float MaxTickDelta = 5.0f;

	// ---- Faim et soif (10.2) ----
	inline constexpr float MaxGauge = 100.f;
	inline constexpr float StartHunger = 100.f;
	inline constexpr float StartThirst = 100.f;
	/** Valeurs à la réapparition (9.2). */
	inline constexpr float RespawnHunger = 60.f;
	inline constexpr float RespawnThirst = 60.f;
	/** Baisse par minute réelle. */
	inline constexpr float HungerLossPerMinute = 1.6f;
	inline constexpr float ThirstLossPerMinute = 2.5f;
	/** Faim ×1,5 sous le seuil de froid ; soif ×2 au-dessus du seuil de chaleur. */
	inline constexpr float ColdHungerMultiplier = 1.5f;
	inline constexpr float HotThirstMultiplier = 2.0f;
	/** Seuil « bas » des deux jauges. */
	inline constexpr float LowGaugeThreshold = 25.f;
	/** Faim < 25 : régénération d'endurance ×0,7. Soif < 25 : endurance max ×0,8. Chaleur > 35 : régénération ×0,8. */
	inline constexpr float LowHungerStaminaRegenMultiplier = 0.7f;
	inline constexpr float LowThirstMaxStaminaMultiplier = 0.8f;
	inline constexpr float HotStaminaRegenMultiplier = 0.8f;
	/** Dégâts par seconde : faim à 0, soif à 0, froid extrême, chaleur extrême. */
	inline constexpr float StarvationDamagePerSecond = 1.f;
	inline constexpr float DehydrationDamagePerSecond = 2.f;
	inline constexpr float FreezingDamagePerSecond = 1.f;
	inline constexpr float HeatstrokeDamagePerSecond = 1.f;

	// ---- Température (10.2) ----
	/** Confort 10-28 °C. Les seuils d'effet ci-dessous sont décalés de ComfortRange (passif) vers l'extérieur. */
	inline constexpr float ComfortMinC = 10.f;
	inline constexpr float ComfortMaxC = 28.f;
	inline constexpr float ColdHungerThresholdC = 5.f;
	inline constexpr float FreezingThresholdC = -10.f;
	inline constexpr float HotThirstThresholdC = 35.f;
	inline constexpr float HeatstrokeThresholdC = 45.f;
	/** Vitesse de lissage de la température ressentie vers sa cible (°C par seconde). */
	inline constexpr float TemperatureSmoothingPerSecond = 1.f;
	/** Température corporelle « neutre » (milieu de la plage de confort) : départ et réapparition. */
	inline constexpr float NeutralTemperatureC = 19.f;
	/** Bornes d'import de sauvegarde et de l'isolation / du décalage météo. */
	inline constexpr float MinFeltTemperatureC = -80.f;
	inline constexpr float MaxFeltTemperatureC = 100.f;
	inline constexpr float MaxInsulationC = 40.f;
	inline constexpr float MaxComfortRangeC = 20.f;

	// ---- Environnement (10.2, 10.4) ----
	inline constexpr float DefaultAmbientTemperatureC = 15.f;
	inline constexpr float DefaultCorruptionRatePerSecond = 0.f;
	inline constexpr float NightTemperatureOffsetC = -8.f;
	inline constexpr float NightCorruptionMultiplier = 2.f;
	inline constexpr float DefaultTimeOfDayHours = 12.f;
	inline constexpr float NightStartHour = 20.f;
	inline constexpr float NightEndHour = 6.f;
	/** Valeurs de référence d'un feu de camp (+15 °C dans 6 m) et d'un brasero purificateur (−1/s dans 8 m). */
	inline constexpr float CampfireHeatRadius = 600.f;
	inline constexpr float CampfireHeatBonusC = 15.f;
	inline constexpr float PurifierRadius = 800.f;
	inline constexpr float PurifierRatePerSecond = 1.f;
	/** Bornes des paramètres enregistrés (robustesse face à une donnée absurde). */
	inline constexpr float MaxWeatherOffsetC = 60.f;
	inline constexpr float MaxHeatBonusC = 100.f;
	inline constexpr float MaxPurifierRate = 10.f;

	// ---- Corruption (10.2) ----
	inline constexpr float MaxCorruption = 100.f;
	/** ≥ 25 : tag State.Survival.Whispers. ≥ 50 : PV max ×0,85. ≥ 75 : PV max ×0,70. 100 : mort. */
	inline constexpr float WhispersThreshold = 25.f;
	inline constexpr float MajorThreshold = 50.f;
	inline constexpr float CriticalThreshold = 75.f;
	inline constexpr float MajorMaxHealthMultiplier = 0.85f;
	inline constexpr float CriticalMaxHealthMultiplier = 0.70f;
	/** Coup de type Corruption sans CorruptionAmount : clamp(0,2 × dégâts, 2, 20). */
	inline constexpr float HitCorruptionFactor = 0.2f;
	inline constexpr float HitCorruptionMin = 2.f;
	inline constexpr float HitCorruptionMax = 20.f;
	/** Part de la Corruption d'un coup qui passe malgré une garde (le bouclier réduit la corruption de 30 %, 4.2). */
	inline constexpr float BlockedCorruptionFactor = 0.7f;
	/** Une sauvegarde ne peut pas restaurer un joueur à 100 (il serait tué au premier tick). */
	inline constexpr float MaxImportedCorruption = 99.f;
	/** Réanimation par un allié : la Corruption est plafonnée à cette valeur (le joueur ne revient pas à 75+ ni à 100). */
	inline constexpr float ReviveCorruptionCap = 60.f;
	/** Réanimation : faim et soif remontées à au moins cette valeur (sinon un joueur mort de faim ou de soif re-meurt aussitôt). */
	inline constexpr float ReviveMinHungerThirst = 30.f;

	// ---- Intoxication et huile ignifuge (10.3) ----
	inline constexpr float DefaultIntoxicationDamagePerSecond = 1.f;
	inline constexpr float DefaultFireOilBonus = 0.25f;
	inline constexpr float MaxFireResistanceBonus = 0.9f;
	/** Bornes d'import et de lecture des lignes de la DataTable. */
	inline constexpr float MaxIntoxicationSeconds = 600.f;
	inline constexpr float MaxFireOilSeconds = 600.f;
	inline constexpr float MaxIntoxicationDamagePerSecond = 20.f;

	// ---- Consommation (10.3) ----
	/** Délai minimal entre deux consommations réussies. */
	inline constexpr float ConsumeDelay = 1.0f;
	/** Tolérance de gigue réseau sur ce délai (l'arrivée des RPC n'est pas régulière). */
	inline constexpr float ConsumeDelayTolerance = 0.1f;
	/** Plafond de RPC Server_Consume traités par seconde (anti-saturation). */
	inline constexpr int32 MaxConsumeRequestsPerSecond = 5;
}

/**
 * État de survie d'un joueur tel qu'il est sauvegardé (contrat 10.4).
 * TOUS les défauts sont à 0 volontairement : une valeur égale au défaut n'est pas écrite par la sérialisation,
 * donc un défaut non nul changerait silencieusement les anciens fichiers au rééquilibrage des constantes.
 * Conséquence : une structure neuve vaut « faim 0, soif 0 » ; l'appelant (sauvegarde v2) ne doit importer
 * que si l'état de survie existe réellement (bHasSurvivalState), sinon utiliser ApplyRespawnValues.
 * Les durées restantes sont en secondes ; l'import les borne (voir UEMBSurvivalComponent::ImportSaveData).
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBSurvivalSaveData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float Hunger = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float Thirst = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float Corruption = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float FeltTemperature = 0.f;

	/** Secondes restantes de l'huile ignifuge (0 = inactive). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float FireOilRemaining = 0.f;

	/** Secondes restantes d'intoxication (0 = saine). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Survival")
	float IntoxicationRemaining = 0.f;
};

/**
 * Ligne de la DataTable des consommables (DT_Consumables, import depuis Data/DT_Consumables.csv).
 * Le nom de ligne est l'identifiant de la ressource de l'inventaire (ex. « Berries »).
 * Les noms de colonnes sont figés : le CSV s'importe tel quel avec ce type de ligne.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBConsumableRow : public FTableRowBase
{
	GENERATED_BODY()

	/** Variation de la faim (positif = nourrit). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable")
	float HungerDelta = 0.f;

	/** Variation de la soif (positif = désaltère). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable")
	float ThirstDelta = 0.f;

	/** Soin en fraction des PV maximum, entre 0 et 1 (0,4 = 40 %), comme UEMBStatsComponent::GetHealthPercent. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0", ClampMax = "1"))
	float HealthPercent = 0.f;

	/** Variation de la Corruption (négatif = purifie). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable")
	float CorruptionDelta = 0.f;

	/** Probabilité (0 à 1) de provoquer une intoxication. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0", ClampMax = "1"))
	float IntoxicationChance = 0.f;

	/** Durée de l'intoxication en secondes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0"))
	float IntoxicationDuration = 0.f;

	/** Dégâts par seconde de l'intoxication. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0"))
	float IntoxicationDamagePerSecond = 0.f;

	/** Vrai : supprime l'intoxication en cours (antidote). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable")
	bool bCuresIntoxication = false;

	/** Résistance au feu temporaire ajoutée (0,25 = +25 %). Non cumulable : un nouvel usage rafraîchit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0", ClampMax = "0.9"))
	float FireResistanceBonus = 0.f;

	/** Durée de la résistance au feu en secondes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable", meta = (ClampMin = "0"))
	float FireResistanceDuration = 0.f;

	/** Texte d'information pour l'interface. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Consumable")
	FText Description;
};
