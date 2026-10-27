// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBEnvironmentVolume.h"
#include "EMBEnvironmentSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

AEMBEnvironmentVolume::AEMBEnvironmentVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false; // Posé dans le niveau : présent sur chaque machine, rien à répliquer.

	BaseTemperatureC = EMBSurvivalDefaults::DefaultAmbientTemperatureC;
	CorruptionRatePerSecond = EMBSurvivalDefaults::DefaultCorruptionRatePerSecond;
	Priority = 0;
	BiomeId = EMBWeather::DefaultBiomeId();

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	SetRootComponent(Box);
	Box->SetBoxExtent(FVector(1000.f, 1000.f, 500.f));
	// Volume purement géométrique : aucune collision ni événement (le test d'appartenance est un calcul).
	Box->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Box->SetGenerateOverlapEvents(false);
	Box->SetCanEverAffectNavigation(false);
}

bool AEMBEnvironmentVolume::ContainsPoint(const FVector& WorldLocation) const
{
	if (!Box)
	{
		return false;
	}

	// Le point est ramené dans le repère local du composant (rotation et échelle annulées) : on le compare
	// donc à l'étendue NON mise à l'échelle.
	const FVector Local = Box->GetComponentTransform().InverseTransformPosition(WorldLocation);
	const FVector Extent = Box->GetUnscaledBoxExtent();
	return FMath::Abs(Local.X) <= Extent.X && FMath::Abs(Local.Y) <= Extent.Y && FMath::Abs(Local.Z) <= Extent.Z;
}

void AEMBEnvironmentVolume::BeginPlay()
{
	Super::BeginPlay();

	if (UWorld* World = GetWorld())
	{
		if (UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>())
		{
			Environment->RegisterVolume(this);
		}
	}
}

void AEMBEnvironmentVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		if (UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>())
		{
			Environment->UnregisterVolume(this);
		}
	}

	Super::EndPlay(EndPlayReason);
}
