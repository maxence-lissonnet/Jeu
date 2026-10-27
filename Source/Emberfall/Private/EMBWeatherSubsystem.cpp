// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBWeatherSubsystem.h"
#include "Emberfall.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBWorldStateReplicator.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "TimerManager.h"

namespace
{
	const TArray<FEMBBiomeWeatherState>& WeatherEmptyBiomes()
	{
		static const TArray<FEMBBiomeWeatherState> Empty;
		return Empty;
	}
}

UEMBWeatherSubsystem::UEMBWeatherSubsystem()
{
	WeatherTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_Weather.DT_Weather")));
	DayDurationSeconds = EMBWeatherDefaults::DayDurationSeconds;
	NightDurationSeconds = EMBWeatherDefaults::NightDurationSeconds;
}

void UEMBWeatherSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Durées de config bornées une seule fois, ici : tous les chemins (réplicateur, repli client, import) lisent
	// ensuite des valeurs sûres, y compris quand le réplicateur n'existe pas encore.
	DayDurationSeconds = EMBWeather::SanitizeWeatherRange(DayDurationSeconds, EMBWeatherDefaults::DayDurationSeconds,
		EMBWeatherDefaults::MinPhaseDurationSeconds, EMBWeatherDefaults::MaxPhaseDurationSeconds);
	NightDurationSeconds = EMBWeather::SanitizeWeatherRange(NightDurationSeconds, EMBWeatherDefaults::NightDurationSeconds,
		EMBWeatherDefaults::MinPhaseDurationSeconds, EMBWeatherDefaults::MaxPhaseDurationSeconds);
}

bool UEMBWeatherSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UEMBWeatherSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (IsServer())
	{
		EnsureInitialized();
	}
	InWorld.GetTimerManager().SetTimer(SyncTimerHandle, this, &UEMBWeatherSubsystem::SyncTick, EMBWeatherDefaults::SyncPeriodSeconds, true);
	RefreshEnvironment();
}

void UEMBWeatherSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SyncTimerHandle);
	}
	Replicator = nullptr;
	LoadedTable = nullptr;
	ServerBiomes.Reset();
	Rows.Reset();
	Super::Deinitialize();
}

bool UEMBWeatherSubsystem::IsServer() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}

double UEMBWeatherSubsystem::GetServerNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

// ---------------------------------------------------------------- Initialisation serveur

void UEMBWeatherSubsystem::EnsureInitialized()
{
	if (bInitialized || !IsServer() || !GetWorld())
	{
		return;
	}
	bInitialized = true;

	// Tirages du serveur seulement : le client ne simule jamais la météo, il lit l'état répliqué.
	Stream.GenerateNewSeed();

	LoadWeatherRows();
	SpawnReplicator();
	if (!Replicator)
	{
		UE_LOG(LogEmberfall, Error, TEXT("UEMBWeatherSubsystem: replicateur non cree, la meteo et l heure ne seront pas repliquees."));
		return;
	}

	const double Now = GetServerNow();
	Replicator->ServerSetTimeReference(EMBWeatherDefaults::StartHour, Now, DayDurationSeconds, NightDurationSeconds);
	BuildInitialBiomes(Now);
	Replicator->ServerSetBiomes(ServerBiomes);
}

void UEMBWeatherSubsystem::LoadWeatherRows()
{
	Rows.Reset();

	LoadedTable = WeatherTable.IsNull() ? nullptr : WeatherTable.LoadSynchronous();
	if (LoadedTable && LoadedTable->GetRowStruct() == FEMBWeatherRow::StaticStruct())
	{
		for (const FName& RowName : LoadedTable->GetRowNames())
		{
			if (Rows.Num() >= EMBWeatherDefaults::MaxBiomes)
			{
				break;
			}
			if (const FEMBWeatherRow* Row = LoadedTable->FindRow<FEMBWeatherRow>(RowName, TEXT("UEMBWeatherSubsystem::LoadWeatherRows"), false))
			{
				Rows.Add(RowName, *Row);
			}
		}
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBWeatherSubsystem: DT_Weather introuvable ou de mauvais type (%s) : meteo temperee par defaut."), *WeatherTable.ToString());
	}

	if (Rows.IsEmpty())
	{
		// Repli : un seul biome tempéré (clair, couvert, pluie), pour que le cycle fonctionne sans données.
		FEMBWeatherRow Fallback;
		Fallback.WeightClear = 5.f;
		Fallback.WeightOvercast = 3.f;
		Fallback.WeightRain = 2.f;
		Rows.Add(EMBWeather::DefaultBiomeId(), Fallback);
	}
}

void UEMBWeatherSubsystem::SpawnReplicator()
{
	if (Replicator)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Replicator = World->SpawnActor<AEMBWorldStateReplicator>(AEMBWorldStateReplicator::StaticClass(), FTransform::Identity, Params);
}

void UEMBWeatherSubsystem::BuildInitialBiomes(double ServerNow)
{
	ServerBiomes.Reset();

	// Ordre stable (tri lexical) : l'ordre d'une TMap n'est pas garanti.
	TArray<FName> Names;
	Rows.GetKeys(Names);
	Names.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });

	for (const FName& BiomeId : Names)
	{
		const FEMBWeatherRow& Row = Rows[BiomeId];
		const EEMBWeatherState Initial = EMBWeather::IsValidState(Row.InitialState) ? Row.InitialState : EEMBWeatherState::Clear;

		FEMBBiomeWeatherState State;
		State.BiomeId = BiomeId;
		State.From = Initial;
		State.To = PickNextState(BiomeId, Initial);
		State.TransitionStartServerTime = ServerNow + Stream.FRandRange(EMBWeatherDefaults::StateHoldMinSeconds, EMBWeatherDefaults::StateHoldMaxSeconds);
		State.TransitionDuration = Stream.FRandRange(EMBWeatherDefaults::TransitionMinSeconds, EMBWeatherDefaults::TransitionMaxSeconds);
		ServerBiomes.Add(State);
	}
}

// ---------------------------------------------------------------- Boucle (1 Hz, toutes machines)

void UEMBWeatherSubsystem::SyncTick()
{
	if (IsServer())
	{
		EnsureInitialized();
		AdvanceWeather(GetServerNow());
	}
	RefreshEnvironment();
}

EEMBWeatherState UEMBWeatherSubsystem::PickNextState(FName BiomeId, EEMBWeatherState Current)
{
	const FEMBWeatherRow* Row = Rows.Find(BiomeId);
	if (!Row)
	{
		return Current;
	}

	// Tirage pondéré en excluant l'état courant (on change toujours d'état) ; poids renormalisés.
	float Total = 0.f;
	for (uint8 Index = 0; Index < EMBWeatherDefaults::StateCount; ++Index)
	{
		const EEMBWeatherState Candidate = static_cast<EEMBWeatherState>(Index);
		if (Candidate != Current)
		{
			Total += Row->GetWeight(Candidate);
		}
	}
	if (Total <= KINDA_SMALL_NUMBER)
	{
		return Current; // Aucun état possible : le biome reste tel quel.
	}

	float Roll = Stream.FRand() * Total;
	EEMBWeatherState Picked = Current;
	for (uint8 Index = 0; Index < EMBWeatherDefaults::StateCount; ++Index)
	{
		const EEMBWeatherState Candidate = static_cast<EEMBWeatherState>(Index);
		const float Weight = (Candidate != Current) ? Row->GetWeight(Candidate) : 0.f;
		if (Weight <= 0.f)
		{
			continue;
		}
		Picked = Candidate;
		Roll -= Weight;
		if (Roll <= 0.f)
		{
			break;
		}
	}
	return Picked;
}

void UEMBWeatherSubsystem::AdvanceWeather(double ServerNow)
{
	if (!Replicator)
	{
		return;
	}

	bool bChanged = false;
	for (FEMBBiomeWeatherState& Biome : ServerBiomes)
	{
		const double TransitionEnd = Biome.TransitionStartServerTime + Biome.TransitionDuration;
		if (ServerNow < TransitionEnd)
		{
			continue;
		}

		// Transition terminée : l'état visé devient l'état stable, et la suivante est planifiée après une tenue de 240 à 600 s.
		// Base = fin de la transition précédente (pas « maintenant ») pour ne pas accumuler de dérive.
		Biome.From = Biome.To;
		Biome.To = PickNextState(Biome.BiomeId, Biome.From);
		Biome.TransitionStartServerTime = TransitionEnd + Stream.FRandRange(EMBWeatherDefaults::StateHoldMinSeconds, EMBWeatherDefaults::StateHoldMaxSeconds);
		Biome.TransitionDuration = Stream.FRandRange(EMBWeatherDefaults::TransitionMinSeconds, EMBWeatherDefaults::TransitionMaxSeconds);
		bChanged = true;
	}

	if (bChanged)
	{
		Replicator->ServerSetBiomes(ServerBiomes);
	}
}

void UEMBWeatherSubsystem::RefreshEnvironment()
{
	const UWorld* World = GetWorld();
	UEMBEnvironmentSubsystem* Environment = World ? World->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr;
	if (!Environment)
	{
		return;
	}

	const float Hours = GetTimeOfDayHours();
	const bool bNight = EMBWeather::IsNightHour(Hours);
	Environment->SetTimeOfDay(Hours);

	// Décalage de température et multiplicateur de Corruption de chaque biome, mélangés pendant les transitions.
	const double Now = GetServerNow();
	for (const FEMBBiomeWeatherState& Biome : (Replicator ? Replicator->GetBiomes() : WeatherEmptyBiomes()))
	{
		const float Alpha = Biome.GetAlpha(Now);
		const float Offset = FMath::Lerp(EMBWeather::GetTemperatureOffset(Biome.From, bNight), EMBWeather::GetTemperatureOffset(Biome.To, bNight), Alpha);
		const float CorruptionMultiplier = FMath::Lerp(EMBWeather::GetCorruptionMultiplier(Biome.From), EMBWeather::GetCorruptionMultiplier(Biome.To), Alpha);
		Environment->SetBiomeWeather(Biome.BiomeId, Offset, CorruptionMultiplier);
	}
}

void UEMBWeatherSubsystem::RegisterReplicator(AEMBWorldStateReplicator* InReplicator)
{
	if (IsValid(InReplicator))
	{
		Replicator = InReplicator;
		RefreshEnvironment();
	}
}

void UEMBWeatherSubsystem::UnregisterReplicator(AEMBWorldStateReplicator* InReplicator)
{
	if (Replicator == InReplicator)
	{
		Replicator = nullptr;
	}
}

// ---------------------------------------------------------------- Requêtes

float UEMBWeatherSubsystem::GetTimeOfDayHours() const
{
	const double Now = GetServerNow();
	if (Replicator)
	{
		return Replicator->ComputeTimeOfDay(Now);
	}
	// Réplicateur pas encore arrivé (client qui se connecte) : estimation depuis le départ d'un monde neuf.
	return EMBWeather::AdvanceHour(EMBWeatherDefaults::StartHour, Now, DayDurationSeconds, NightDurationSeconds);
}

bool UEMBWeatherSubsystem::IsNight() const
{
	return EMBWeather::IsNightHour(GetTimeOfDayHours());
}

FName UEMBWeatherSubsystem::GetBiomeIdAt(const FVector& Location) const
{
	const UWorld* World = GetWorld();
	const UEMBEnvironmentSubsystem* Environment = World ? World->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr;
	return Environment ? Environment->GetBiomeIdAt(Location) : EMBWeather::DefaultBiomeId();
}

const FEMBBiomeWeatherState* UEMBWeatherSubsystem::FindBiomeState(FName BiomeId) const
{
	// O(n) sur les biomes : n est de l'ordre de la dizaine.
	const TArray<FEMBBiomeWeatherState>& Biomes = Replicator ? Replicator->GetBiomes() : WeatherEmptyBiomes();
	const FEMBBiomeWeatherState* Fallback = nullptr;
	for (const FEMBBiomeWeatherState& Biome : Biomes)
	{
		if (Biome.BiomeId == BiomeId)
		{
			return &Biome;
		}
		if (Biome.BiomeId == EMBWeather::DefaultBiomeId())
		{
			Fallback = &Biome;
		}
	}
	return Fallback; // Biome inconnu de la table : météo du biome par défaut.
}

const FEMBBiomeWeatherState* UEMBWeatherSubsystem::FindBiomeStateAt(const FVector& Location) const
{
	return FindBiomeState(GetBiomeIdAt(Location));
}

void UEMBWeatherSubsystem::GetWeatherBlendAt(const FVector& Location, EEMBWeatherState& OutFrom, EEMBWeatherState& OutTo, float& OutAlpha) const
{
	const FEMBBiomeWeatherState* Biome = FindBiomeStateAt(Location);
	if (!Biome)
	{
		OutFrom = EEMBWeatherState::Clear;
		OutTo = EEMBWeatherState::Clear;
		OutAlpha = 0.f;
		return;
	}
	OutFrom = Biome->From;
	OutTo = Biome->To;
	OutAlpha = Biome->GetAlpha(GetServerNow());
}

EEMBWeatherState UEMBWeatherSubsystem::GetWeatherAt(const FVector& Location) const
{
	EEMBWeatherState From, To;
	float Alpha;
	GetWeatherBlendAt(Location, From, To, Alpha);
	return Alpha < 0.5f ? From : To;
}

float UEMBWeatherSubsystem::GetVisibilityMultiplierAt(const FVector& Location) const
{
	EEMBWeatherState From, To;
	float Alpha;
	GetWeatherBlendAt(Location, From, To, Alpha);
	return FMath::Lerp(EMBWeather::GetVisibilityMultiplier(From), EMBWeather::GetVisibilityMultiplier(To), Alpha);
}

// ---------------------------------------------------------------- Heure

void UEMBWeatherSubsystem::SetTimeOfDay(float Hours)
{
	if (!IsServer() || !FMath::IsFinite(Hours))
	{
		return;
	}
	EnsureInitialized();
	if (Replicator)
	{
		Replicator->ServerSetTimeReference(Hours, GetServerNow(), DayDurationSeconds, NightDurationSeconds);
		RefreshEnvironment();
	}
}

// ---------------------------------------------------------------- Sauvegarde

bool UEMBWeatherSubsystem::ExportSaveData(FEMBWeatherSaveData& Out) const
{
	// Réplicateur absent ou biomes pas encore créés : il n'y a rien de fiable à écrire.
	if (!Replicator || Replicator->GetBiomes().IsEmpty())
	{
		return false;
	}

	Out.bInitialized = true;
	Out.TimeOfDayHours = GetTimeOfDayHours();
	Out.Biomes.Reset();

	const double Now = GetServerNow();
	for (const FEMBBiomeWeatherState& Biome : (Replicator ? Replicator->GetBiomes() : WeatherEmptyBiomes()))
	{
		FEMBBiomeWeatherSave Save;
		Save.BiomeId = Biome.BiomeId;
		Save.Current = Biome.From;
		Save.Target = Biome.To;
		Save.TransitionDuration = Biome.TransitionDuration;
		// Temps relatifs : le temps serveur n'a plus de sens après un rechargement.
		if (Now < Biome.TransitionStartServerTime)
		{
			Save.SecondsUntilTransition = static_cast<float>(Biome.TransitionStartServerTime - Now);
		}
		else
		{
			Save.TransitionElapsed = static_cast<float>(FMath::Min<double>(Now - Biome.TransitionStartServerTime, Biome.TransitionDuration));
		}
		Out.Biomes.Add(Save);
	}
	return true;
}

void UEMBWeatherSubsystem::ImportSaveData(const FEMBWeatherSaveData& In)
{
	if (!IsServer())
	{
		return;
	}
	EnsureInitialized();
	// Structure jamais renseignée (monde sans météo sauvegardée) : le monde garde son départ à 08 h.
	if (!In.bInitialized || !Replicator)
	{
		return;
	}

	// Une sauvegarde est une entrée non fiable : tout est borné, les biomes inconnus sont ignorés.
	const double Now = GetServerNow();
	float Hours = EMBWeather::SanitizeWeatherRange(In.TimeOfDayHours, EMBWeatherDefaults::StartHour, 0.f, 24.f);
	if (Hours >= 24.f)
	{
		Hours = 0.f;
	}
	Replicator->ServerSetTimeReference(Hours, Now, DayDurationSeconds, NightDurationSeconds);

	const int32 Limit = FMath::Min(In.Biomes.Num(), EMBWeatherDefaults::MaxBiomes);
	for (int32 Index = 0; Index < Limit; ++Index)
	{
		const FEMBBiomeWeatherSave& Save = In.Biomes[Index];
		FEMBBiomeWeatherState* Biome = ServerBiomes.FindByPredicate([&Save](const FEMBBiomeWeatherState& Entry) { return Entry.BiomeId == Save.BiomeId; });
		if (!Biome || !EMBWeather::IsValidState(Save.Current) || !EMBWeather::IsValidState(Save.Target))
		{
			continue;
		}

		const float Duration = EMBWeather::SanitizeWeatherRange(Save.TransitionDuration, EMBWeatherDefaults::TransitionMinSeconds, EMBWeatherDefaults::TransitionMinSeconds, EMBWeatherDefaults::TransitionMaxSeconds);
		const float Elapsed = EMBWeather::SanitizeWeatherRange(Save.TransitionElapsed, 0.f, 0.f, Duration);
		// Transition déjà commencée : plus d'attente. Sinon on reprend le compte à rebours restant.
		const float Until = Elapsed > 0.f ? 0.f : EMBWeather::SanitizeWeatherRange(Save.SecondsUntilTransition, 0.f, 0.f, EMBWeatherDefaults::StateHoldMaxSeconds);

		Biome->From = Save.Current;
		Biome->To = Save.Target;
		Biome->TransitionDuration = Duration;
		Biome->TransitionStartServerTime = Now + Until - Elapsed;
	}

	Replicator->ServerSetBiomes(ServerBiomes);
	RefreshEnvironment();
}
