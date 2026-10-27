// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBEnvironmentSubsystem.h"
#include "EMBEnvironmentVolume.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

bool UEMBEnvironmentSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// Mondes de jeu seulement (PIE, jeu packagé, serveur) : pas d'aperçus d'éditeur ni de mondes d'asset.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UEMBEnvironmentSubsystem::Deinitialize()
{
	HeatSources.Reset();
	Purifiers.Reset();
	Volumes.Reset();
	BiomeWeather.Reset();
	Super::Deinitialize();
}

// ---------------------------------------------------------------- Requêtes

const AEMBEnvironmentVolume* UEMBEnvironmentSubsystem::FindBestVolumeAt(const FVector& Location) const
{
	// O(n) sur les volumes : n est de l'ordre de la dizaine (biomes du monde).
	const AEMBEnvironmentVolume* Best = nullptr;
	for (const TWeakObjectPtr<AEMBEnvironmentVolume>& Weak : Volumes)
	{
		const AEMBEnvironmentVolume* Volume = Weak.Get();
		if (!IsValid(Volume) || !Volume->ContainsPoint(Location))
		{
			continue;
		}
		// Strictement supérieure : à priorité égale, le premier enregistré garde la place (résultat stable).
		if (!Best || Volume->Priority > Best->Priority)
		{
			Best = Volume;
		}
	}
	return Best;
}

float UEMBEnvironmentSubsystem::GetAmbientTemperatureAt(const FVector& Location) const
{
	const AEMBEnvironmentVolume* Volume = FindBestVolumeAt(Location);
	float Temperature = Volume ? Volume->BaseTemperatureC : EMBSurvivalDefaults::DefaultAmbientTemperatureC;
	if (IsNight())
	{
		Temperature += EMBSurvivalDefaults::NightTemperatureOffsetC;
	}
	// Météo globale (setter historique) + météo du biome (poussée par UEMBWeatherSubsystem).
	const FName BiomeId = Volume ? Volume->BiomeId : EMBWeather::DefaultBiomeId();
	return Temperature + WeatherTemperatureOffsetC + GetBiomeWeather(BiomeId).TemperatureOffsetC;
}

FName UEMBEnvironmentSubsystem::GetBiomeIdAt(const FVector& Location) const
{
	const AEMBEnvironmentVolume* Volume = FindBestVolumeAt(Location);
	return (Volume && !Volume->BiomeId.IsNone()) ? Volume->BiomeId : EMBWeather::DefaultBiomeId();
}

UEMBEnvironmentSubsystem::FBiomeWeather UEMBEnvironmentSubsystem::GetBiomeWeather(FName BiomeId) const
{
	// Repli sur le biome par défaut : un volume au BiomeId absent de DT_Weather subit la météo « Temperate ».
	if (const FBiomeWeather* Found = BiomeWeather.Find(BiomeId))
	{
		return *Found;
	}
	if (const FBiomeWeather* Fallback = BiomeWeather.Find(EMBWeather::DefaultBiomeId()))
	{
		return *Fallback;
	}
	return FBiomeWeather();
}

void UEMBEnvironmentSubsystem::SetBiomeWeather(FName BiomeId, float TemperatureOffsetC, float CorruptionMultiplier)
{
	if (BiomeId.IsNone() || !FMath::IsFinite(TemperatureOffsetC) || !FMath::IsFinite(CorruptionMultiplier))
	{
		return;
	}

	FBiomeWeather Updated;
	Updated.TemperatureOffsetC = FMath::Clamp(TemperatureOffsetC, -EMBSurvivalDefaults::MaxWeatherOffsetC, EMBSurvivalDefaults::MaxWeatherOffsetC);
	Updated.CorruptionMultiplier = FMath::Clamp(CorruptionMultiplier, 0.f, 10.f);

	const FBiomeWeather* Existing = BiomeWeather.Find(BiomeId);
	// Le mélange d'une transition change ces valeurs chaque seconde : on ne prévient qu'au changement visible
	// (1 °C ou 0,1 de multiplicateur), pas à chaque mise à jour.
	const bool bVisibleChange = !Existing
		|| FMath::RoundToInt(Existing->TemperatureOffsetC) != FMath::RoundToInt(Updated.TemperatureOffsetC)
		|| FMath::RoundToInt(Existing->CorruptionMultiplier * 10.f) != FMath::RoundToInt(Updated.CorruptionMultiplier * 10.f);

	BiomeWeather.Add(BiomeId, Updated);
	if (bVisibleChange)
	{
		BroadcastChanged();
	}
}

float UEMBEnvironmentSubsystem::GetCorruptionRateAt(const FVector& Location) const
{
	const AEMBEnvironmentVolume* Volume = FindBestVolumeAt(Location);
	const float Rate = Volume ? FMath::Max(0.f, Volume->CorruptionRatePerSecond) : EMBSurvivalDefaults::DefaultCorruptionRatePerSecond;
	// Brume corrompue du biome (x1,5) et nuit (x2) se multiplient.
	const FName BiomeId = Volume ? Volume->BiomeId : EMBWeather::DefaultBiomeId();
	const float WeatherMultiplier = GetBiomeWeather(BiomeId).CorruptionMultiplier;
	return (IsNight() ? Rate * EMBSurvivalDefaults::NightCorruptionMultiplier : Rate) * WeatherMultiplier;
}

bool UEMBEnvironmentSubsystem::IsNight() const
{
	return TimeOfDayHours >= EMBSurvivalDefaults::NightStartHour || TimeOfDayHours < EMBSurvivalDefaults::NightEndHour;
}

float UEMBEnvironmentSubsystem::GetHeatBonusAt(const FVector& Location) const
{
	// Non cumulable : le feu le plus chaud l'emporte (deux feux côte à côte ne donnent pas +30 °C).
	return GetStrongestValueAt(HeatSources, Location);
}

float UEMBEnvironmentSubsystem::GetPurificationRateAt(const FVector& Location) const
{
	// Non cumulable également : empile-t-on des braseros pour annuler toute Corruption ? Non, le plus fort seul compte.
	return GetStrongestValueAt(Purifiers, Location);
}

float UEMBEnvironmentSubsystem::GetStrongestValueAt(const TArray<FRadiusSource>& Sources, const FVector& Location)
{
	// O(n) sur les sources enregistrées (quelques dizaines au plus).
	float Strongest = 0.f;
	for (const FRadiusSource& Entry : Sources)
	{
		const AActor* Actor = Entry.Source.Get();
		if (!IsValid(Actor) || Entry.Value <= Strongest)
		{
			continue;
		}
		if (FVector::DistSquared(Actor->GetActorLocation(), Location) <= FMath::Square(Entry.Radius))
		{
			Strongest = Entry.Value;
		}
	}
	return Strongest;
}

// ---------------------------------------------------------------- Setters

void UEMBEnvironmentSubsystem::SetTimeOfDay(float Hours)
{
	if (!FMath::IsFinite(Hours))
	{
		return;
	}

	float Wrapped = FMath::Fmod(Hours, 24.f);
	if (Wrapped < 0.f)
	{
		Wrapped += 24.f;
	}

	const bool bWasNight = IsNight();
	TimeOfDayHours = Wrapped;
	if (bWasNight != IsNight())
	{
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::SetWeatherTemperatureOffset(float DeltaC)
{
	if (!FMath::IsFinite(DeltaC))
	{
		return;
	}

	const float Clamped = FMath::Clamp(DeltaC, -EMBSurvivalDefaults::MaxWeatherOffsetC, EMBSurvivalDefaults::MaxWeatherOffsetC);
	if (!FMath::IsNearlyEqual(Clamped, WeatherTemperatureOffsetC))
	{
		WeatherTemperatureOffsetC = Clamped;
		BroadcastChanged();
	}
}

// ---------------------------------------------------------------- Sources

bool UEMBEnvironmentSubsystem::UpsertSource(TArray<FRadiusSource>& Sources, AActor* Source, float Radius, float Value)
{
	bool bChanged = Sources.RemoveAll([](const FRadiusSource& Entry) { return !Entry.Source.IsValid(); }) > 0;

	FRadiusSource* Existing = Sources.FindByPredicate([Source](const FRadiusSource& Entry) { return Entry.Source.Get() == Source; });
	if (Existing)
	{
		if (!FMath::IsNearlyEqual(Existing->Radius, Radius) || !FMath::IsNearlyEqual(Existing->Value, Value))
		{
			Existing->Radius = Radius;
			Existing->Value = Value;
			bChanged = true;
		}
		return bChanged;
	}

	FRadiusSource NewEntry;
	NewEntry.Source = Source;
	NewEntry.Radius = Radius;
	NewEntry.Value = Value;
	Sources.Add(NewEntry);
	return true;
}

bool UEMBEnvironmentSubsystem::RemoveSource(TArray<FRadiusSource>& Sources, const AActor* Source)
{
	return Sources.RemoveAll([Source](const FRadiusSource& Entry)
	{
		return !Entry.Source.IsValid() || Entry.Source.Get() == Source;
	}) > 0;
}

void UEMBEnvironmentSubsystem::RegisterHeatSource(AActor* Source, float Radius, float BonusC)
{
	if (!IsValid(Source) || !FMath::IsFinite(Radius) || !FMath::IsFinite(BonusC))
	{
		return;
	}
	const float SafeRadius = FMath::Max(0.f, Radius);
	const float SafeBonus = FMath::Clamp(BonusC, 0.f, EMBSurvivalDefaults::MaxHeatBonusC);
	if (UpsertSource(HeatSources, Source, SafeRadius, SafeBonus))
	{
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::UnregisterHeatSource(AActor* Source)
{
	if (RemoveSource(HeatSources, Source))
	{
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::RegisterPurifier(AActor* Source, float Radius, float RatePerSecond)
{
	if (!IsValid(Source) || !FMath::IsFinite(Radius) || !FMath::IsFinite(RatePerSecond))
	{
		return;
	}
	const float SafeRadius = FMath::Max(0.f, Radius);
	const float SafeRate = FMath::Clamp(RatePerSecond, 0.f, EMBSurvivalDefaults::MaxPurifierRate);
	if (UpsertSource(Purifiers, Source, SafeRadius, SafeRate))
	{
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::UnregisterPurifier(AActor* Source)
{
	if (RemoveSource(Purifiers, Source))
	{
		BroadcastChanged();
	}
}

// ---------------------------------------------------------------- Volumes

void UEMBEnvironmentSubsystem::RegisterVolume(AEMBEnvironmentVolume* Volume)
{
	if (!IsValid(Volume))
	{
		return;
	}

	Volumes.RemoveAll([](const TWeakObjectPtr<AEMBEnvironmentVolume>& Entry) { return !Entry.IsValid(); });
	const bool bAlreadyRegistered = Volumes.ContainsByPredicate([Volume](const TWeakObjectPtr<AEMBEnvironmentVolume>& Entry) { return Entry.Get() == Volume; });
	if (!bAlreadyRegistered)
	{
		Volumes.Add(Volume);
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::UnregisterVolume(AEMBEnvironmentVolume* Volume)
{
	const int32 Removed = Volumes.RemoveAll([Volume](const TWeakObjectPtr<AEMBEnvironmentVolume>& Entry)
	{
		return !Entry.IsValid() || Entry.Get() == Volume;
	});
	if (Removed > 0)
	{
		BroadcastChanged();
	}
}

void UEMBEnvironmentSubsystem::BroadcastChanged()
{
	OnEnvironmentChanged.Broadcast();
}
