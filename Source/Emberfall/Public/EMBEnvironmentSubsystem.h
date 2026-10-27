// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMBSurvivalTypes.h"
#include "EMBWeatherTypes.h"
#include "EMBEnvironmentSubsystem.generated.h"

class AActor;
class AEMBEnvironmentVolume;

/**
 * Diffusé quand l'environnement change de façon structurelle : passage jour/nuit, décalage météo modifié,
 * volume de biome, source de chaleur ou purificateur ajouté/retiré. PAS à chaque mise à jour de l'heure
 * (le sous-système météo appelle SetTimeOfDay chaque seconde : seule la bascule jour/nuit est diffusée).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMBOnEnvironmentChanged);

/**
 * Environnement du monde pour la survie (contrat 10.4) : température ambiante, nuit, taux de Corruption,
 * sources de chaleur et purificateurs.
 *
 * Alimenté par les AEMBEnvironmentVolume (biomes) et par UEMBWeatherSubsystem, qui pousse chaque seconde sur
 * chaque machine l'heure (SetTimeOfDay) et la météo de chaque biome (SetBiomeWeather : décalage de température,
 * multiplicateur de Corruption). SetWeatherTemperatureOffset reste un décalage global additionnel.
 *
 * Réseau : sous-système local à chaque machine, sans réplication. Les consommateurs (UEMBSurvivalComponent)
 * l'interrogent côté serveur ; l'heure et la météo doivent y être poussées par le serveur (et par les
 * clients qui en auraient besoin pour l'affichage).
 *
 * Coût : toutes les requêtes parcourent les listes (volumes, sources, purificateurs) en O(n). n reste petit
 * (quelques biomes, quelques dizaines de feux) et chaque joueur interroge à 1 Hz : pas de structure spatiale.
 */
UCLASS()
class EMBERFALL_API UEMBEnvironmentSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------- Cycle de vie
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	// ---------------------------------------------------------------- Requêtes (contrat 10.4)

	/** Température ambiante en °C : biome (volume de plus haute priorité, sinon 15 °C) + nuit (−8 °C) + décalage météo. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetAmbientTemperatureAt(const FVector& Location) const;

	/** Corruption par seconde au point donné : taux du biome (sinon 0), ×2 la nuit. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetCorruptionRateAt(const FVector& Location) const;

	/** Nuit de 20 h à 6 h (6 h exclu). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	bool IsNight() const;

	/** Bonus de chaleur en °C : le plus fort des feux dont le rayon contient le point (non cumulable). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetHeatBonusAt(const FVector& Location) const;

	/** Purification par seconde : le plus fort des purificateurs dont le rayon contient le point (non cumulable). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetPurificationRateAt(const FVector& Location) const;

	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetTimeOfDay() const { return TimeOfDayHours; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	float GetWeatherTemperatureOffset() const { return WeatherTemperatureOffsetC; }

	// ---------------------------------------------------------------- Setters (appelés par le sous-système météo / jour-nuit)

	/** Heure du jour en heures (0-24, ramenée dans l'intervalle ; défaut 12 h). Valeur non finie ignorée. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void SetTimeOfDay(float Hours);

	/** Décalage de température dû à la météo, en °C (borné à ±60). Valeur non finie ignorée. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void SetWeatherTemperatureOffset(float DeltaC);

	// ---------------------------------------------------------------- Sources enregistrées

	/** Enregistre (ou met à jour) une source de chaleur : rayon en cm, bonus en °C. La position suit l'acteur. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void RegisterHeatSource(AActor* Source, float Radius, float BonusC);

	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void UnregisterHeatSource(AActor* Source);

	/** Enregistre (ou met à jour) un purificateur : rayon en cm, taux de réduction de Corruption par seconde. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void RegisterPurifier(AActor* Source, float Radius, float RatePerSecond);

	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void UnregisterPurifier(AActor* Source);

	// ---------------------------------------------------------------- Météo par biome (v1.4, poussée par UEMBWeatherSubsystem)

	/**
	 * Décalage de température (°C) et multiplicateur du taux de Corruption de zone propres à un biome (la météo
	 * de ce biome). Valeurs non finies ignorées, décalage borné à ±60, multiplicateur à [0, 10].
	 * Diffuse OnEnvironmentChanged seulement si l'arrondi (1 °C, 0,1) change : pas à chaque seconde d'une transition.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Environment")
	void SetBiomeWeather(FName BiomeId, float TemperatureOffsetC, float CorruptionMultiplier);

	/** Biome au point donné : BiomeId du volume de plus haute priorité, sinon « Temperate ». */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Environment")
	FName GetBiomeIdAt(const FVector& Location) const;

	// ---------------------------------------------------------------- Volumes de biome

	/** Appelés par AEMBEnvironmentVolume (BeginPlay / EndPlay). */
	void RegisterVolume(AEMBEnvironmentVolume* Volume);
	void UnregisterVolume(AEMBEnvironmentVolume* Volume);

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Environment")
	FEMBOnEnvironmentChanged OnEnvironmentChanged;

private:
	/** Source ponctuelle à rayon (chaleur ou purification). Référence faible : un acteur détruit sans désinscription est ignoré. */
	struct FRadiusSource
	{
		TWeakObjectPtr<AActor> Source;
		float Radius = 0.f;
		float Value = 0.f;
	};

	/** Volume de plus haute priorité contenant le point ; à priorité égale, le premier enregistré. nullptr si aucun. */
	const AEMBEnvironmentVolume* FindBestVolumeAt(const FVector& Location) const;

	/** Plus forte valeur parmi les sources dont le rayon contient le point (0 si aucune). */
	static float GetStrongestValueAt(const TArray<FRadiusSource>& Sources, const FVector& Location);

	/** Ajoute ou met à jour une source ; purge les entrées invalides. Retourne true si la liste a changé. */
	static bool UpsertSource(TArray<FRadiusSource>& Sources, AActor* Source, float Radius, float Value);

	/** Retire les entrées de l'acteur (et les invalides). Retourne true si la liste a changé. */
	static bool RemoveSource(TArray<FRadiusSource>& Sources, const AActor* Source);

	/** Effet de la météo d'un biome. Neutre par défaut. */
	struct FBiomeWeather
	{
		float TemperatureOffsetC = 0.f;
		float CorruptionMultiplier = 1.f;
	};

	/** Météo du biome (repli : « Temperate », sinon neutre). */
	FBiomeWeather GetBiomeWeather(FName BiomeId) const;

	void BroadcastChanged();

	/** Météo par biome, mise à jour par le sous-système météo (au plus quelques entrées). */
	TMap<FName, FBiomeWeather> BiomeWeather;

	float TimeOfDayHours = EMBSurvivalDefaults::DefaultTimeOfDayHours;
	float WeatherTemperatureOffsetC = 0.f;

	TArray<FRadiusSource> HeatSources;
	TArray<FRadiusSource> Purifiers;
	TArray<TWeakObjectPtr<AEMBEnvironmentVolume>> Volumes;
};
