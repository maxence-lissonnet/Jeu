// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCreatureSpawner.h"

#include "Emberfall.h"
#include "EMBBossBase.h"
#include "EMBCreatureBase.h"
#include "EMBCreatureSignificance.h"
#include "EMBCreatureTypes.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBTerritorialDragon.h"
#include "EMBWeatherSubsystem.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "TimerManager.h"

AEMBCreatureSpawner::AEMBCreatureSpawner()
{
	PrimaryActorTick.bCanEverTick = false;

	// Logique serveur uniquement : ni réplication, ni copie chez les clients.
	bReplicates = false;
	bNetLoadOnClient = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AEMBCreatureSpawner::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		return;
	}

	ResolveArchetype();

	// Premier passage décalé au hasard : de nombreux spawners ne doivent pas évaluer le même instant.
	GetWorldTimerManager().SetTimer(EvaluateTimerHandle, this, &AEMBCreatureSpawner::EvaluateSpawn,
		EvaluationInterval, true, FMath::FRandRange(0.f, EvaluationInterval));
}

void AEMBCreatureSpawner::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(EvaluateTimerHandle);
	Super::EndPlay(EndPlayReason);
}

void AEMBCreatureSpawner::ResolveArchetype()
{
	bArchetypeIsCreature = ArchetypeClass && ArchetypeClass->IsChildOf(AEMBCreatureBase::StaticClass());
	if (!ArchetypeClass)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBCreatureSpawner %s : ArchetypeClass non assignee, aucune apparition."), *GetName());
		return;
	}

	GroupSizeMin = 1;
	GroupSizeMax = 1;
	bNightBoost = false;

	// Un archétype boss (Sylvadrac) est unique : jamais plus d'un exemplaire, quel que soit le réglage de l'instance.
	if (!bArchetypeIsCreature)
	{
		MaxAlive = 1;
	}

	FEMBCreatureRow Row;
	if (bArchetypeIsCreature && AEMBCreatureBase::FindRowForClass(ArchetypeClass, Row))
	{
		GroupSizeMin = FMath::Max(1, Row.GroupSizeMin);
		GroupSizeMax = FMath::Max(GroupSizeMin, Row.GroupSizeMax);
		bNightBoost = Row.bNightBoost;
	}
}

void AEMBCreatureSpawner::InitializeAsRespawner(TSubclassOf<AActor> InArchetypeClass, const float DelaySeconds, const float InActivationDistance, const float InMinPlayerDistance)
{
	ArchetypeClass = InArchetypeClass;
	MaxAlive = 1;
	SpawnRadius = 0.f;
	GroupSpread = 0.f;
	RespawnInterval = DelaySeconds;
	ActivationDistance = InActivationDistance;
	MinPlayerDistance = InMinPlayerDistance;

	const UWorld* World = GetWorld();
	NextSpawnTime = (World ? World->GetTimeSeconds() : 0.0) + static_cast<double>(DelaySeconds);
}

// ----------------------------------------------------------------------------------------------
// Conditions
// ----------------------------------------------------------------------------------------------

bool AEMBCreatureSpawner::IsNightNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (const UEMBWeatherSubsystem* Weather = World->GetSubsystem<UEMBWeatherSubsystem>())
	{
		return Weather->IsNight();
	}
	// Repli : l'environnement connaît aussi la nuit (setter appelé par le futur système jour/nuit).
	const UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>();
	return Environment && Environment->IsNight();
}

float AEMBCreatureSpawner::GetEffectiveInterval() const
{
	// Un seul levier de nuit (MaxAlive x 2, voir EvaluateSpawn) : l'intervalle ne change pas.
	return RespawnInterval;
}

bool AEMBCreatureSpawner::HasPlayerWithin(const TArray<APawn*>& Players, const FVector& Center, const float Radius) const
{
	const double RadiusSquared = FMath::Square(static_cast<double>(Radius));
	for (const APawn* Player : Players)
	{
		if (FVector::DistSquared(Player->GetActorLocation(), Center) <= RadiusSquared)
		{
			return true;
		}
	}
	return false;
}

bool AEMBCreatureSpawner::PickSpawnLocation(const FVector& Center, const float Radius, FVector& OutLocation) const
{
	if (Radius <= 1.f)
	{
		OutLocation = Center;
		return true;
	}

	// Point atteignable sur le navmesh (les créatures patrouillent et poursuivent par pathfinding).
	if (UNavigationSystemV1* NavSystem = UNavigationSystemV1::GetCurrent(GetWorld()))
	{
		FNavLocation Result;
		if (NavSystem->GetRandomReachablePointInRadius(Center, Radius, Result))
		{
			OutLocation = Result.Location;
			return true;
		}
	}

	// Repli sans navmesh : point tiré uniformément dans le disque, à la hauteur du centre.
	const float Angle = FMath::FRandRange(0.f, 2.f * PI);
	const float Distance = Radius * FMath::Sqrt(FMath::FRand());
	OutLocation = Center + FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, 0.f);
	return true;
}

// ----------------------------------------------------------------------------------------------
// Apparition
// ----------------------------------------------------------------------------------------------

void AEMBCreatureSpawner::PruneAlive()
{
	Alive.RemoveAll([](const TWeakObjectPtr<AActor>& Entry) { return !Entry.IsValid(); });
}

void AEMBCreatureSpawner::EvaluateSpawn()
{
	UWorld* World = GetWorld();
	if (!World || !ArchetypeClass)
	{
		return;
	}
	PruneAlive();

	const double Now = World->GetTimeSeconds();
	const bool bBoost = bNightBoost && IsNightNow();

	// Poids de nuit x2 : on peut entretenir deux fois plus de créatures corrompues, et plus vite.
	const int32 EffectiveMax = bBoost
		? FMath::CeilToInt(static_cast<float>(MaxAlive) * EMBCreatureDefaults::NightSpawnWeightMultiplier)
		: MaxAlive;
	if (Alive.Num() >= EffectiveMax || Now < NextSpawnTime)
	{
		return;
	}

	// Un joueur vivant doit être assez proche pour que l'apparition serve à quelque chose.
	TArray<APawn*> Players;
	AEMBBossBase::CollectAlivePlayerPawns(World, Players);
	if (!HasPlayerWithin(Players, GetActorLocation(), ActivationDistance))
	{
		return;
	}

	// Taille de la vague, bornée par la place du spawner puis par le budget serveur de 40 créatures vivantes.
	int32 WaveSize = FMath::RandRange(GroupSizeMin, GroupSizeMax);
	int32 Room = EffectiveMax - Alive.Num();
	if (bArchetypeIsCreature)
	{
		if (const UEMBCreatureSignificance* Significance = World->GetSubsystem<UEMBCreatureSignificance>())
		{
			Room = FMath::Min(Room, EMBCreatureDefaults::ServerBudget - Significance->GetLiveCreatureCount());
		}
	}
	// Pas de groupe incomplet : une vague de goules a au moins GroupSizeMin membres.
	if (Room < GroupSizeMin)
	{
		return;
	}
	WaveSize = FMath::Min(WaveSize, Room);

	// Point d'ancrage de la vague : aucun joueur à moins de MinPlayerDistance (on réessaiera au prochain passage).
	FVector Anchor;
	if (!PickSpawnLocation(GetActorLocation(), SpawnRadius, Anchor) || HasPlayerWithin(Players, Anchor, MinPlayerDistance))
	{
		return;
	}

	int32 SpawnedCount = 0;
	for (int32 Index = 0; Index < WaveSize; ++Index)
	{
		FVector Location = Anchor;
		if (Index > 0)
		{
			PickSpawnLocation(Anchor, GroupSpread, Location);
		}
		if (AActor* NewActor = SpawnOne(Location))
		{
			Alive.Add(NewActor);
			++SpawnedCount;
		}
	}

	if (SpawnedCount > 0)
	{
		NextSpawnTime = Now + static_cast<double>(GetEffectiveInterval());
	}
}

AActor* AEMBCreatureSpawner::SpawnOne(const FVector& Location)
{
	UWorld* World = GetWorld();
	if (!World || !ArchetypeClass)
	{
		return nullptr;
	}

	// La capsule est posée au-dessus du point navigable (le navmesh est au niveau du sol).
	const float HalfHeight = ArchetypeClass->GetDefaultObject<AActor>()->GetSimpleCollisionHalfHeight();
	const FRotator Rotation(0.f, FMath::FRandRange(0.f, 360.f), 0.f);

	// Projection au sol : le nid d'un dragon est l'emplacement de l'ancien acteur (capsule à mi-hauteur), pas le sol.
	FVector GroundLocation = Location;
	FHitResult GroundHit;
	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(EMBSpawnerGround), false, this);
	if (World->LineTraceSingleByChannel(GroundHit, Location + FVector(0.f, 0.f, 1000.f), Location - FVector(0.f, 0.f, 6000.f), ECC_Visibility, TraceParams))
	{
		GroundLocation = GroundHit.ImpactPoint;
	}
	const FTransform Transform(Rotation, GroundLocation + FVector(0.f, 0.f, HalfHeight));

	// Apparition différée : le lien avec le spawner doit exister avant le BeginPlay de la créature.
	AActor* Spawned = World->SpawnActorDeferred<AActor>(ArchetypeClass, Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!Spawned)
	{
		return nullptr;
	}

	if (AEMBCreatureBase* Creature = Cast<AEMBCreatureBase>(Spawned))
	{
		Creature->SetOwningSpawner(this);
	}
	else if (AEMBTerritorialDragon* Dragon = Cast<AEMBTerritorialDragon>(Spawned))
	{
		Dragon->SetOwningSpawner(this);
	}

	Spawned->FinishSpawning(Transform);
	return IsValid(Spawned) ? Spawned : nullptr;
}

// ----------------------------------------------------------------------------------------------
// Notifications des enfants
// ----------------------------------------------------------------------------------------------

void AEMBCreatureSpawner::NotifyChildDied(AActor* Child, const float OverrideRespawnDelay)
{
	Alive.RemoveAll([Child](const TWeakObjectPtr<AActor>& Entry) { return Entry.Get() == Child; });

	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const float Delay = OverrideRespawnDelay >= 0.f
		? OverrideRespawnDelay
		: GetEffectiveInterval();

	// max : plusieurs morts rapprochées (un groupe de goules) repoussent la vague suivante après la dernière.
	NextSpawnTime = FMath::Max(NextSpawnTime, World->GetTimeSeconds() + static_cast<double>(Delay));
}

void AEMBCreatureSpawner::NotifyChildReturned(AActor* Child)
{
	Alive.RemoveAll([Child](const TWeakObjectPtr<AActor>& Entry) { return Entry.Get() == Child; });
}
