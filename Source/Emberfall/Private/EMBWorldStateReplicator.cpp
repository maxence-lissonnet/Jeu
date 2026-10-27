// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBWorldStateReplicator.h"
#include "EMBWeatherSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "Net/UnrealNetwork.h"

AEMBWorldStateReplicator::AEMBWorldStateReplicator()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true; // Tous les clients en ont besoin, quelle que soit la distance.
	SetReplicateMovement(false);
	// L'état change rarement : une mise à jour par seconde au plus (ForceNetUpdate à chaque vrai changement).
	SetNetUpdateFrequency(1.f);
	SetMinNetUpdateFrequency(0.2f);
}

void AEMBWorldStateReplicator::BeginPlay()
{
	Super::BeginPlay();

	// Sur toutes les machines : le sous-système local lit ce réplicateur.
	if (UWorld* World = GetWorld())
	{
		if (UEMBWeatherSubsystem* Weather = World->GetSubsystem<UEMBWeatherSubsystem>())
		{
			Weather->RegisterReplicator(this);
		}
	}
}

void AEMBWorldStateReplicator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UEMBWeatherSubsystem* Weather = World->GetSubsystem<UEMBWeatherSubsystem>())
		{
			Weather->UnregisterReplicator(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBWorldStateReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AEMBWorldStateReplicator, ReferenceHour);
	DOREPLIFETIME(AEMBWorldStateReplicator, ReferenceServerTime);
	DOREPLIFETIME(AEMBWorldStateReplicator, DayDurationSeconds);
	DOREPLIFETIME(AEMBWorldStateReplicator, NightDurationSeconds);
	DOREPLIFETIME(AEMBWorldStateReplicator, Biomes);
}

double AEMBWorldStateReplicator::GetServerTimeNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	// Sur le serveur GetServerWorldTimeSeconds renvoie le temps monde ; sur un client, une estimation synchronisée.
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

float AEMBWorldStateReplicator::ComputeTimeOfDay(double ServerNow) const
{
	return EMBWeather::AdvanceHour(ReferenceHour, ServerNow - ReferenceServerTime, DayDurationSeconds, NightDurationSeconds);
}

void AEMBWorldStateReplicator::ServerSetTimeReference(float Hour, double ServerTime, float InDayDurationSeconds, float InNightDurationSeconds)
{
	if (!HasAuthority())
	{
		return;
	}

	float SafeHour = FMath::IsFinite(Hour) ? FMath::Fmod(Hour, 24.f) : EMBWeatherDefaults::StartHour;
	if (SafeHour < 0.f)
	{
		SafeHour += 24.f;
	}
	ReferenceHour = SafeHour;
	ReferenceServerTime = ServerTime;
	DayDurationSeconds = FMath::IsFinite(InDayDurationSeconds)
		? FMath::Clamp(InDayDurationSeconds, EMBWeatherDefaults::MinPhaseDurationSeconds, EMBWeatherDefaults::MaxPhaseDurationSeconds)
		: EMBWeatherDefaults::DayDurationSeconds;
	NightDurationSeconds = FMath::IsFinite(InNightDurationSeconds)
		? FMath::Clamp(InNightDurationSeconds, EMBWeatherDefaults::MinPhaseDurationSeconds, EMBWeatherDefaults::MaxPhaseDurationSeconds)
		: EMBWeatherDefaults::NightDurationSeconds;

	ForceNetUpdate();
}

void AEMBWorldStateReplicator::ServerSetBiomes(const TArray<FEMBBiomeWeatherState>& NewBiomes)
{
	if (!HasAuthority())
	{
		return;
	}

	Biomes = NewBiomes;
	if (Biomes.Num() > EMBWeatherDefaults::MaxBiomes)
	{
		Biomes.SetNum(EMBWeatherDefaults::MaxBiomes);
	}
	ForceNetUpdate();
}

void AEMBWorldStateReplicator::OnRep_TimeReference()
{
	NotifySubsystem();
}

void AEMBWorldStateReplicator::OnRep_Biomes()
{
	NotifySubsystem();
}

void AEMBWorldStateReplicator::NotifySubsystem() const
{
	if (const UWorld* World = GetWorld())
	{
		if (UEMBWeatherSubsystem* Weather = World->GetSubsystem<UEMBWeatherSubsystem>())
		{
			Weather->RefreshEnvironment();
		}
	}
}
