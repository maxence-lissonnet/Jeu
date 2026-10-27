// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBFallingTree.h"

#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBCombatTypes.h"
#include "EMBResourceNodeTypes.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Math/RandomStream.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AEMBFallingTree::AEMBFallingTree()
{
	// Aucun Tick : la physique est pilotée par Chaos, les dégâts par l'événement de contact.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	// Mouvement NON répliqué (voir le commentaire de classe) : simulation locale à chaque machine.
	SetReplicateMovement(false);
	SetNetUpdateFrequency(2.f);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetCollisionProfileName(UCollisionProfile::PhysicsActor_ProfileName);
	Mesh->SetGenerateOverlapEvents(false);
	// Nécessaire pour recevoir OnComponentHit d'un corps simulé.
	Mesh->SetNotifyRigidBodyCollision(true);
	// Un tronc de 10 s ne justifie pas de reconstruire la navigation.
	Mesh->SetCanEverAffectNavigation(false);
}

void AEMBFallingTree::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// Une seule fois à l'ouverture du canal : la valeur ne change plus après InitFall.
	DOREPLIFETIME_CONDITION(AEMBFallingTree, FallInit, COND_InitialOnly);
}

void AEMBFallingTree::InitFall(UStaticMesh* InMesh, const FVector& InFallDirection, const int32 InSeed)
{
	if (!HasAuthority())
	{
		return;
	}
	FVector Direction = InFallDirection.GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}
	FallInit.Mesh = InMesh;
	FallInit.FallDirection = FVector_NetQuantizeNormal(Direction);
	FallInit.Seed = static_cast<uint8>(FMath::Clamp(InSeed, 0, 255));
	FallInit.bValid = (InMesh != nullptr);
}

void AEMBFallingTree::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// Disparition pilotée par le serveur : la destruction se réplique aux clients.
		SetLifeSpan(EMBHarvestDefaults::FallingTreeLifetimeSeconds);
		Mesh->OnComponentHit.AddDynamic(this, &AEMBFallingTree::OnMeshHit);
		bDamageWindowOpen = true;
		GetWorldTimerManager().SetTimer(DamageWindowTimerHandle, this, &AEMBFallingTree::EndDamageWindow,
			EMBHarvestDefaults::FallingTreeDamageWindowSeconds, false);
	}
	// Client : la destruction décidée par le serveur se réplique (aucune durée de vie locale).

	// Serveur : les paramètres sont déjà posés par InitFall. Client : OnRep_FallInit a normalement déjà lancé la
	// simulation ; sinon (ordre d'arrivée), on la lance ici.
	if (FallInit.bValid)
	{
		StartSimulation();
	}
}

void AEMBFallingTree::OnRep_FallInit()
{
	if (FallInit.bValid)
	{
		StartSimulation();
	}
}

void AEMBFallingTree::StartSimulation()
{
	if (bSimulationStarted || !FallInit.bValid || !FallInit.Mesh)
	{
		return;
	}
	bSimulationStarted = true;

	Mesh->SetStaticMesh(FallInit.Mesh);
	Mesh->SetSimulatePhysics(true);
	// Masse fixée : indépendante du volume du mesh, donc de l'échelle de l'arbre, pour une chute prévisible.
	Mesh->SetMassOverrideInKg(NAME_None, EMBHarvestDefaults::FallingTreeMassKg, true);

	// Direction de chute + petit écart déterministe (même graine partout).
	FRandomStream Random(FallInit.Seed);
	const float YawJitterDegrees = Random.FRandRange(-8.f, 8.f);
	const FVector FallDirection = FVector(FallInit.FallDirection).RotateAngleAxis(YawJitterDegrees, FVector::UpVector);

	// L'impulsion s'applique au sommet de l'arbre pour le faire basculer autour de sa base.
	const FBoxSphereBounds MeshBounds = Mesh->Bounds;
	const FVector TopLocation = MeshBounds.Origin + FVector(0.f, 0.f, MeshBounds.BoxExtent.Z * 0.8f);
	const FVector Impulse = FallDirection * Mesh->GetMass() * EMBHarvestDefaults::FallingTreeImpulseSpeed;
	Mesh->AddImpulseAtLocation(Impulse, TopLocation);

	GetWorldTimerManager().SetTimer(SleepTimerHandle, this, &AEMBFallingTree::PutToSleep,
		EMBHarvestDefaults::FallingTreeSleepSeconds, false);
}

void AEMBFallingTree::PutToSleep()
{
	// Coupe le coût de simulation du reste de la vie de l'arbre (un contact peut le réveiller).
	Mesh->PutRigidBodyToSleep();
}

void AEMBFallingTree::EndDamageWindow()
{
	bDamageWindowOpen = false;
}

void AEMBFallingTree::OnMeshHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit)
{
	// Dégâts : serveur uniquement, pendant la chute, et une seule fois par pawn.
	if (!HasAuthority() || !bDamageWindowOpen || !OtherActor || !OtherActor->IsA<APawn>())
	{
		return;
	}
	if (DamagedActors.Contains(OtherActor))
	{
		return;
	}

	const FVector Velocity = Mesh->GetPhysicsLinearVelocity();
	if (Velocity.SizeSquared() < FMath::Square(EMBHarvestDefaults::FallingTreeMinHitSpeed))
	{
		return; // arbre quasiment immobile : il ne blesse pas un pawn qui s'appuie dessus
	}
	DamagedActors.Add(OtherActor);

	FEMBHitData HitData;
	HitData.Instigator = this;
	HitData.Damage = EMBHarvestDefaults::FallingTreeDamage;
	HitData.PoiseDamage = EMBHarvestDefaults::FallingTreePoise;
	HitData.DamageType = EEMBDamageType::Physical;
	HitData.HitLocation = Hit.ImpactPoint;
	HitData.HitDirection = Velocity.GetSafeNormal();
	// Un tronc de plusieurs centaines de kilos ne se pare pas ni ne se bloque ; l'esquive (i-frames) reste possible.
	HitData.bUnblockable = true;
	HitData.bUnparryable = true;

	UEMBCombatComponent::ApplyHitToActor(OtherActor, HitData);
}
