// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EMBSurvivalTypes.h"
#include "EMBWeatherTypes.generated.h"

/** États météo (référentiel section 15). L'ordre est figé : il sert de format de sauvegarde. */
UENUM(BlueprintType)
enum class EEMBWeatherState : uint8
{
	Clear,
	Overcast,
	Rain,
	Storm,
	Fog,
	Snow,
	CorruptedMist,
	AshStorm
};

/**
 * Constantes du cycle jour/nuit et de la météo (référentiel section 15).
 * Valeurs de conception à équilibrer en playtest : ce ne sont pas des mesures.
 */
namespace EMBWeatherDefaults
{
	/** Journée de 48 min réelles : jour (06 h -> 20 h) en 32 min, nuit (20 h -> 06 h) en 16 min. */
	inline constexpr float DayDurationSeconds = 32.f * 60.f;
	inline constexpr float NightDurationSeconds = 16.f * 60.f;
	/** Heure de départ d'un monde neuf. */
	inline constexpr float StartHour = 8.f;
	/** Bornes de durée (config) : évitent une division par zéro ou un cycle absurde. */
	inline constexpr float MinPhaseDurationSeconds = 10.f;
	inline constexpr float MaxPhaseDurationSeconds = 36000.f;

	/** Durée d'un état stable avant la transition suivante : tirée entre ces bornes (secondes). */
	inline constexpr float StateHoldMinSeconds = 240.f;
	inline constexpr float StateHoldMaxSeconds = 600.f;
	/** Durée d'une transition (mélange linéaire) : tirée entre ces bornes (secondes). */
	inline constexpr float TransitionMinSeconds = 90.f;
	inline constexpr float TransitionMaxSeconds = 180.f;

	/** Nombre d'états (EEMBWeatherState) ; borne les valeurs lues dans une sauvegarde. */
	inline constexpr uint8 StateCount = 8;
	/** Bornes de robustesse : biomes lus dans la table ou dans une sauvegarde. */
	inline constexpr int32 MaxBiomes = 64;

	/** Période de synchronisation de l'environnement (heure, décalages par biome), en secondes. */
	inline constexpr float SyncPeriodSeconds = 1.f;
	/** Majoration de la Corruption de zone sous la brume corrompue. */
	inline constexpr float CorruptedMistCorruptionMultiplier = 1.5f;
	/** Bonus de température d'un ciel dégagé pendant le jour. */
	inline constexpr float ClearDayBonusC = 2.f;
}

namespace EMBWeather
{
	/** Biome par défaut d'un volume (et des zones hors volume). FName construit une seule fois. */
	inline FName DefaultBiomeId()
	{
		static const FName Id(TEXT("Temperate"));
		return Id;
	}

	/** Valeur bornée ; une valeur non finie retombe sur Fallback. */
	inline float SanitizeWeatherRange(float Value, float Fallback, float Min, float Max)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
	}

	inline bool IsValidState(EEMBWeatherState State)
	{
		return static_cast<uint8>(State) < EMBWeatherDefaults::StateCount;
	}

	/** Décalage de température en °C (référentiel 15). Clear : +2 le jour seulement. */
	inline float GetTemperatureOffset(EEMBWeatherState State, bool bNight)
	{
		switch (State)
		{
		case EEMBWeatherState::Clear:         return bNight ? 0.f : EMBWeatherDefaults::ClearDayBonusC;
		case EEMBWeatherState::Overcast:      return -2.f;
		case EEMBWeatherState::Rain:          return -4.f;
		case EEMBWeatherState::Storm:         return -6.f;
		case EEMBWeatherState::Fog:           return -1.f;
		case EEMBWeatherState::Snow:          return -10.f;
		case EEMBWeatherState::CorruptedMist: return -3.f;
		case EEMBWeatherState::AshStorm:      return 6.f;
		default:                              return 0.f;
		}
	}

	/** Multiplicateur de visibilité (perception des créatures et du joueur). */
	inline float GetVisibilityMultiplier(EEMBWeatherState State)
	{
		switch (State)
		{
		case EEMBWeatherState::Fog:           return 0.6f;
		case EEMBWeatherState::Storm:         return 0.7f;
		case EEMBWeatherState::Rain:          return 0.85f;
		case EEMBWeatherState::AshStorm:      return 0.5f;
		case EEMBWeatherState::CorruptedMist: return 0.6f;
		default:                              return 1.f;
		}
	}

	/** Multiplicateur du taux de Corruption de zone. */
	inline float GetCorruptionMultiplier(EEMBWeatherState State)
	{
		return State == EEMBWeatherState::CorruptedMist ? EMBWeatherDefaults::CorruptedMistCorruptionMultiplier : 1.f;
	}

	/**
	 * Conversions heure <-> position dans le cycle de 48 min. Le jour (06 h -> 20 h, 14 h de jeu) et la nuit
	 * (20 h -> 06 h, 10 h de jeu) n'ont pas la même vitesse : on raisonne en « secondes depuis l'aube ».
	 * Calcul fermé (sans boucle), sûr pour de très grands écarts de temps.
	 */
	inline double HourToCycleSeconds(float Hour, float DaySeconds, float NightSeconds)
	{
		const float DayHours = EMBSurvivalDefaults::NightStartHour - EMBSurvivalDefaults::NightEndHour;
		const float NightHours = 24.f - DayHours;
		float SinceDawn = FMath::Fmod(Hour - EMBSurvivalDefaults::NightEndHour, 24.f);
		if (SinceDawn < 0.f)
		{
			SinceDawn += 24.f;
		}
		if (SinceDawn < DayHours)
		{
			return static_cast<double>(SinceDawn / DayHours) * DaySeconds;
		}
		return DaySeconds + static_cast<double>((SinceDawn - DayHours) / NightHours) * NightSeconds;
	}

	inline float CycleSecondsToHour(double CycleSeconds, float DaySeconds, float NightSeconds)
	{
		const double Cycle = static_cast<double>(DaySeconds) + NightSeconds;
		double Wrapped = FMath::Fmod(CycleSeconds, Cycle);
		if (Wrapped < 0.0)
		{
			Wrapped += Cycle;
		}
		const float DayHours = EMBSurvivalDefaults::NightStartHour - EMBSurvivalDefaults::NightEndHour;
		const float NightHours = 24.f - DayHours;
		const double SinceDawn = Wrapped < DaySeconds
			? Wrapped / DaySeconds * DayHours
			: DayHours + (Wrapped - DaySeconds) / NightSeconds * NightHours;
		return static_cast<float>(FMath::Fmod(EMBSurvivalDefaults::NightEndHour + SinceDawn, 24.0));
	}

	/** Heure après ElapsedSeconds (négatif accepté : écart de synchronisation d'horloge). */
	inline float AdvanceHour(float StartHour, double ElapsedSeconds, float DaySeconds, float NightSeconds)
	{
		return CycleSecondsToHour(HourToCycleSeconds(StartHour, DaySeconds, NightSeconds) + ElapsedSeconds, DaySeconds, NightSeconds);
	}

	inline bool IsNightHour(float Hour)
	{
		return Hour >= EMBSurvivalDefaults::NightStartHour || Hour < EMBSurvivalDefaults::NightEndHour;
	}
}

/**
 * État météo d'un biome tel qu'il est répliqué : état stable `From`, état visé `To`, début (temps serveur) et
 * durée de la transition. Avant `TransitionStartServerTime` le biome reste en `From` ; ensuite le mélange est
 * linéaire jusqu'à `To`. Le client calcule le mélange localement : rien n'est répliqué à l'image.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBBiomeWeatherState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Weather")
	FName BiomeId;

	UPROPERTY(BlueprintReadOnly, Category = "Weather")
	EEMBWeatherState From = EEMBWeatherState::Clear;

	UPROPERTY(BlueprintReadOnly, Category = "Weather")
	EEMBWeatherState To = EEMBWeatherState::Clear;

	/** Temps serveur (synchronisé) du début de la transition ; peut être dans le futur. */
	UPROPERTY(BlueprintReadOnly, Category = "Weather")
	double TransitionStartServerTime = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "Weather")
	float TransitionDuration = 0.f;

	/** Avancement du mélange From -> To à l'instant ServerNow, entre 0 et 1. */
	float GetAlpha(double ServerNow) const
	{
		if (TransitionDuration <= KINDA_SMALL_NUMBER)
		{
			return 1.f;
		}
		return static_cast<float>(FMath::Clamp((ServerNow - TransitionStartServerTime) / TransitionDuration, 0.0, 1.0));
	}
};

/** État d'un biome dans une sauvegarde. Les temps sont RELATIFS (le temps serveur repart de zéro au chargement). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBBiomeWeatherSave
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	FName BiomeId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	EEMBWeatherState Current = EEMBWeatherState::Clear;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	EEMBWeatherState Target = EEMBWeatherState::Clear;

	/** Secondes avant le début de la transition (0 si elle est déjà commencée). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	float SecondsUntilTransition = 0.f;

	/** Secondes déjà écoulées dans la transition en cours (0 si elle n'a pas commencé). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	float TransitionElapsed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	float TransitionDuration = 0.f;
};

/**
 * Météo et heure sauvegardées. Défauts neutres (0 / vide) : une valeur égale au défaut n'est pas écrite.
 * `bInitialized` distingue « jamais sauvegardé » (structure par défaut, heure 0 = minuit !) d'un vrai état :
 * l'import ignore une structure non initialisée et le monde démarre alors à 08 h.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBWeatherSaveData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	bool bInitialized = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	float TimeOfDayHours = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Weather")
	TArray<FEMBBiomeWeatherSave> Biomes;
};

/**
 * Ligne de DT_Weather (import depuis Data/DT_Weather.csv) : une ligne par biome, le nom de ligne est le
 * `BiomeId` des volumes d'environnement. Les poids sont les probabilités RELATIVES de passer vers chaque état
 * à la prochaine transition ; l'état courant est exclu du tirage (on change toujours d'état) et les poids sont
 * renormalisés. Un poids à 0 interdit l'état dans ce biome.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBWeatherRow : public FTableRowBase
{
	GENERATED_BODY()

	/** État au démarrage d'un monde neuf. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather")
	EEMBWeatherState InitialState = EEMBWeatherState::Clear;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightClear = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightOvercast = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightRain = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightStorm = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightFog = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightSnow = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightCorruptedMist = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weather", meta = (ClampMin = "0"))
	float WeightAshStorm = 0.f;

	/** Poids de transition vers l'état donné (valeur négative ou non finie ramenée à 0). */
	float GetWeight(EEMBWeatherState State) const
	{
		float Weight = 0.f;
		switch (State)
		{
		case EEMBWeatherState::Clear:         Weight = WeightClear; break;
		case EEMBWeatherState::Overcast:      Weight = WeightOvercast; break;
		case EEMBWeatherState::Rain:          Weight = WeightRain; break;
		case EEMBWeatherState::Storm:         Weight = WeightStorm; break;
		case EEMBWeatherState::Fog:           Weight = WeightFog; break;
		case EEMBWeatherState::Snow:          Weight = WeightSnow; break;
		case EEMBWeatherState::CorruptedMist: Weight = WeightCorruptedMist; break;
		case EEMBWeatherState::AshStorm:      Weight = WeightAshStorm; break;
		default: break;
		}
		return (FMath::IsFinite(Weight) && Weight > 0.f) ? Weight : 0.f;
	}
};
