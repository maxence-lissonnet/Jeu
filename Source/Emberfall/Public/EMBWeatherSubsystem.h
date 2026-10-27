// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "Math/RandomStream.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMBWeatherTypes.h"
#include "EMBWeatherSubsystem.generated.h"

class AEMBWorldStateReplicator;
class UDataTable;
class UEMBEnvironmentSubsystem;

/**
 * Cycle jour/nuit et météo par biome (contrat 12 et 15). Autorité serveur ; l'état est répliqué par
 * AEMBWorldStateReplicator et lu par chaque machine à travers ce sous-système.
 *
 *  - HORLOGE : journée de 48 min (jour 32, nuit 16), monde neuf à 08 h. L'heure n'est jamais répliquée à l'image :
 *    elle est recalculée à partir d'une référence serveur (voir le réplicateur).
 *  - MÉTÉO : chaque biome (BiomeId d'un AEMBEnvironmentVolume, défaut « Temperate ») a son état. Il reste stable
 *    240 à 600 s, puis passe à un état tiré selon les poids de DT_Weather pendant 90 à 180 s (mélange linéaire).
 *    Tirages par FRandomStream côté serveur seulement.
 *  - ENVIRONNEMENT : toutes les secondes, sur chaque machine, l'heure et les décalages par biome (température,
 *    multiplicateur de Corruption) sont poussés vers UEMBEnvironmentSubsystem, que la survie interroge.
 *
 * Initialisation paresseuse et idempotente (EnsureInitialized) : la sauvegarde peut appeler ImportSaveData avant ou
 * après OnWorldBeginPlay sans dépendre de l'ordre d'initialisation.
 *
 * Configuration (DefaultGame.ini), section [/Script/Emberfall.EMBWeatherSubsystem] :
 *   WeatherTable=/Game/Data/DT_Weather.DT_Weather
 *   DayDurationSeconds=1920.0
 *   NightDurationSeconds=960.0
 */
UCLASS(Config = Game)
class EMBERFALL_API UEMBWeatherSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UEMBWeatherSubsystem();

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

	// ---------------------------------------------------------------- Contrat 12

	/** Heure du jour en heures (0-24). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Weather")
	float GetTimeOfDayHours() const;

	/** Nuit de 20 h à 6 h. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Weather")
	bool IsNight() const;

	/** État dominant au point donné (celui des deux états du mélange qui pèse le plus). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Weather")
	EEMBWeatherState GetWeatherAt(const FVector& Location) const;

	/** Visibilité (perception) au point donné : 1 = normale, 0,5 = tempête de cendres, interpolée pendant les transitions. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Weather")
	float GetVisibilityMultiplierAt(const FVector& Location) const;

	/** Retourne false (Out inchangé) si le réplicateur ou l'état des biomes n'est pas prêt : ne pas marquer l'état comme sauvegardé. */
	bool ExportSaveData(FEMBWeatherSaveData& Out) const;

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Serveur. Import borné : une structure non initialisée est ignorée, les biomes inconnus aussi. */
	void ImportSaveData(const FEMBWeatherSaveData& In);

	// ---------------------------------------------------------------- Extensions

	/** Les deux états du mélange au point donné et l'avancement (0 = From, 1 = To) : pour le ciel. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Weather")
	void GetWeatherBlendAt(const FVector& Location, EEMBWeatherState& OutFrom, EEMBWeatherState& OutTo, float& OutAlpha) const;

	/** Serveur : fixe l'heure du jour (commande de debug, cinématique). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Weather")
	void SetTimeOfDay(float Hours);

	/** Pousse sans attendre l'heure et les décalages par biome vers l'environnement (appelé par le réplicateur). */
	void RefreshEnvironment();

	void RegisterReplicator(AEMBWorldStateReplicator* InReplicator);
	void UnregisterReplicator(AEMBWorldStateReplicator* InReplicator);

	/** DataTable des biomes (lignes FEMBWeatherRow, nom de ligne = BiomeId). Lue sur le serveur seulement. */
	UPROPERTY(Config, BlueprintReadOnly, Category = "Weather")
	TSoftObjectPtr<UDataTable> WeatherTable;

	/** Durée du jour (06 h -> 20 h) et de la nuit (20 h -> 06 h) en secondes réelles. */
	UPROPERTY(Config, BlueprintReadOnly, Category = "Weather")
	float DayDurationSeconds;

	UPROPERTY(Config, BlueprintReadOnly, Category = "Weather")
	float NightDurationSeconds;

private:
	bool IsServer() const;
	double GetServerNow() const;

	/** Serveur : charge la table, spawne le réplicateur, crée les états initiaux. Idempotent. */
	void EnsureInitialized();
	void LoadWeatherRows();
	void BuildInitialBiomes(double ServerNow);
	void SpawnReplicator();

	void SyncTick();
	void AdvanceWeather(double ServerNow);
	EEMBWeatherState PickNextState(FName BiomeId, EEMBWeatherState Current);

	/** État du biome (repli : « Temperate », sinon nullptr). */
	const FEMBBiomeWeatherState* FindBiomeState(FName BiomeId) const;
	const FEMBBiomeWeatherState* FindBiomeStateAt(const FVector& Location) const;
	FName GetBiomeIdAt(const FVector& Location) const;

	UPROPERTY(Transient) TObjectPtr<AEMBWorldStateReplicator> Replicator;
	UPROPERTY(Transient) TObjectPtr<UDataTable> LoadedTable;

	/** Copie serveur de l'état des biomes (la référence : le réplicateur en reçoit une copie à chaque changement). */
	TArray<FEMBBiomeWeatherState> ServerBiomes;
	/** Lignes de la table copiées au chargement (poids de transition). */
	TMap<FName, FEMBWeatherRow> Rows;

	FRandomStream Stream;
	FTimerHandle SyncTimerHandle;
	bool bInitialized = false;
};
