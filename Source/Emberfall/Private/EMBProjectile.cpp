// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBProjectile.h"

#include "EMBCombatComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Pas de simulation maximal du mouvement (s) : 4 000 cm/s x 1/60 s = 67 cm par sous-pas, inférieur au diamètre cumulé sphère + capsule. */
	constexpr float MaxSimulationStep = 1.f / 60.f;
	constexpr int32 MaxSimulationIterations = 8;

	/** Rayon de collision par défaut avant le premier tir (cm). */
	constexpr float DefaultCollisionRadius = 25.f;

	/** Durée de vie minimale d'un projectile renvoyé (s) : il doit pouvoir rejoindre un boss en mouvement. */
	constexpr float MinReflectLifetime = 1.5f;
	constexpr float ReflectLifetimeMargin = 1.0f;
}

AEMBProjectile::AEMBProjectile()
{
	// Aucun Tick d'acteur : le mouvement est porté par UProjectileMovementComponent.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false); // la trajectoire est simulée localement à partir de FEMBProjectileNetState
	SetNetUpdateFrequency(10.f);

	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(DefaultCollisionRadius);
	SetRootComponent(Collision);

	// Sphère de requête : pawns en Overlap (la résolution se fait par ApplyHitToActor, qui sait esquiver, garder, parer),
	// décor en Block (pilier, cloche, mur), tout le reste ignoré. Aucun contact physique.
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Collision->SetCollisionObjectType(EMBProjectileDefaults::ObjectChannel); // canal d'objet dédié EMBProjectile
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(EMBProjectileDefaults::ObjectChannel, ECR_Ignore); // les projectiles ne s'arrêtent pas entre eux
	Collision->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Collision->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore); // clones, cloches mobiles : jamais un obstacle
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(false); // activé sur le serveur seulement (BeginPlay) : les clients ne résolvent rien
	Collision->CanCharacterStepUpOn = ECB_No;

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Collision;
	Movement->InitialSpeed = 0.f;               // la vitesse est posée au tir (Launch / simulation client)
	Movement->MaxSpeed = 0.f;                   // 0 : pas de plafond de vitesse
	Movement->ProjectileGravityScale = 0.f;     // ligne droite
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
	Movement->bInitialVelocityInLocalSpace = false;
	// Sous-pas forcés : le résultat ne dépend plus de la fréquence du serveur (garantie anti-tunneling).
	Movement->bForceSubStepping = true;
	Movement->MaxSimulationTimeStep = MaxSimulationStep;
	Movement->MaxSimulationIterations = MaxSimulationIterations;
}

void AEMBProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBProjectile, NetState);
}

void AEMBProjectile::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Branchés partout : les gestionnaires ignorent les clients (aucun dégât n'est résolu hors serveur).
	Collision->OnComponentBeginOverlap.AddUniqueDynamic(this, &AEMBProjectile::HandleBeginOverlap);
	Movement->OnProjectileStop.AddUniqueDynamic(this, &AEMBProjectile::HandleProjectileStop);
}

void AEMBProjectile::BeginPlay()
{
	Super::BeginPlay();

	Collision->SetGenerateOverlapEvents(HasAuthority());

	// Un projectile neuf du pool est inactif. Sur un client qui rejoint en cours de tir, l'état répliqué peut déjà
	// être actif : on ne l'écrase pas (OnRep_NetState s'en charge).
	if (!NetState.bActive)
	{
		StopMotion();
	}
}

void AEMBProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LifetimeTimer);
	}
	Super::EndPlay(EndPlayReason);
}

// ----------------------------------------------------------------------------------------------
// Mouvement
// ----------------------------------------------------------------------------------------------

void AEMBProjectile::StartMotion(const FVector& InOrigin, const FVector& InVelocity)
{
	SetVisualsVisible(true);

	// Collision coupée pendant la téléportation, puis réactivée : le moteur recalcule alors les chevauchements à la
	// nouvelle position (un joueur déjà dans la sphère au départ est touché).
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetActorLocationAndRotation(InOrigin, InVelocity.Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);

	// Un arrêt (OnProjectileStop) détache le composant mis à jour : on le rattache à chaque réutilisation.
	Movement->SetUpdatedComponent(Collision);
	Movement->Velocity = InVelocity;
	Movement->UpdateComponentVelocity();
	Movement->SetComponentTickEnabled(true);
	Movement->Activate(true);
}

void AEMBProjectile::StopMotion()
{
	// Mouvement réellement désactivé (plus de tick, plus de composant mis à jour) ; StartMotion le réactive.
	Movement->StopMovementImmediately();
	Movement->SetUpdatedComponent(nullptr);
	Movement->SetComponentTickEnabled(false);
	Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetVisualsVisible(false);
}

void AEMBProjectile::SetVisualsVisible(const bool bVisible)
{
	// Pas de SetActorHiddenInGame : bHidden est répliqué et ferait sortir l'acteur de la pertinence réseau. L'acteur reste
	// pertinent (les RepNotify de fin et de relance arrivent toujours) ; seuls les composants visuels sont masqués.
	TInlineComponentArray<UPrimitiveComponent*> Primitives(this);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		if (Primitive != Collision)
		{
			Primitive->SetVisibility(bVisible, true);
		}
	}
}

double AEMBProjectile::GetSyncedTime() const
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

void AEMBProjectile::PublishState(const bool bActive, const bool bReflected, const FVector& InOrigin, const FVector& InVelocity)
{
	NetState.bActive = bActive;
	NetState.bReflected = bReflected;
	NetState.Sequence = static_cast<uint8>(NetState.Sequence + 1);
	NetState.Style = CurrentStyle;
	NetState.Origin = InOrigin;
	NetState.Velocity = InVelocity;
	NetState.ServerTime = GetSyncedTime();
	ForceNetUpdate(); // un tir ne doit pas attendre le prochain cycle de réplication (10 Hz)
}

void AEMBProjectile::ArmLifetimeTimer(const float Seconds)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(LifetimeTimer, this, &AEMBProjectile::ReturnToPool, FMath::Max(Seconds, 0.05f), false);
	}
}

// ----------------------------------------------------------------------------------------------
// Tir et retour au pool (serveur)
// ----------------------------------------------------------------------------------------------

void AEMBProjectile::Launch(const FEMBProjectileLaunchParams& Params)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World)
	{
		return;
	}

	World->GetTimerManager().ClearTimer(LifetimeTimer);
	ResolvedActors.Reset();
	ReflectTarget.Reset();

	HitData = Params.Hit;
	bReflectOnPerfectParry = Params.bReflectOnPerfectParry;
	if (Params.ReflectDamage > 0.f)
	{
		ReflectDamage = Params.ReflectDamage;
	}
	ReflectSpeed = Params.ReflectSpeed;
	CurrentSpeed = Params.Speed;
	CurrentStyle = Params.Style;
	OriginBoss = Params.Hit.Instigator.Get();

	// Le tireur n'arrête jamais son propre projectile au balayage (il en est souvent à l'intérieur au départ).
	Collision->SetSphereRadius(Params.CollisionRadius);
	Collision->ClearMoveIgnoreActors();
	if (AActor* Source = Params.Hit.Instigator)
	{
		Collision->IgnoreActorWhenMoving(Source, true);
	}

	FVector Direction = Params.Direction.GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = FVector::ForwardVector;
	}
	const FVector Velocity = Direction * Params.Speed;

	// État local fixé AVANT StartMotion : la réactivation de la collision déclenche les chevauchements immédiatement.
	PublishState(true, false, Params.Origin, Velocity);
	StartMotion(Params.Origin, Velocity);
	NotifyLaunched();
	ArmLifetimeTimer(Params.Lifetime);
}

void AEMBProjectile::ReturnToPool()
{
	if (!HasAuthority() || !NetState.bActive)
	{
		return; // idempotent : plusieurs causes de fin possibles dans la même image (impact + expiration)
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LifetimeTimer);
	}

	StopMotion();
	ResolvedActors.Reset();
	ReflectTarget.Reset();
	HitData = FEMBHitData();
	OriginBoss.Reset();

	PublishState(false, false, GetActorLocation(), FVector::ZeroVector);
	NotifyEnded();
}

// ----------------------------------------------------------------------------------------------
// Collisions (serveur)
// ----------------------------------------------------------------------------------------------

void AEMBProjectile::HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	const int32 OtherBodyIndex, const bool bFromSweep, const FHitResult& SweepResult)
{
	if (!HasAuthority() || !NetState.bActive || !OtherActor)
	{
		return;
	}

	// Projectile renvoyé : seul son ancien tireur est une cible.
	if (NetState.bReflected)
	{
		if (OtherActor == ReflectTarget.Get())
		{
			ResolveHitOnReflectTarget(OtherActor);
		}
		return;
	}

	// Projectile normal : uniquement les joueurs (ni le boss, ni ses clones, ni un autre pawn).
	APawn* Pawn = Cast<APawn>(OtherActor);
	if (!Pawn || !Pawn->IsPlayerControlled())
	{
		return;
	}
	ResolveHitOnPlayer(Pawn);
}

void AEMBProjectile::HandleProjectileStop(const FHitResult& ImpactResult)
{
	// Obstacle du décor (bloquant) : fin de vie. Le mouvement s'est déjà arrêté de lui-même.
	if (HasAuthority())
	{
		ReturnToPool();
	}
}

void AEMBProjectile::ResolveHitOnPlayer(APawn* Player)
{
	// Une seule résolution par joueur et par vol : un joueur qui esquive puis reste dans la sphère n'est pas retouché.
	bool bAlreadyResolved = false;
	ResolvedActors.Add(Player, &bAlreadyResolved);
	if (bAlreadyResolved)
	{
		return;
	}

	FEMBHitData Hit = HitData;
	Hit.HitLocation = GetActorLocation();
	Hit.HitDirection = Movement->Velocity.GetSafeNormal();

	const EEMBHitResult Result = UEMBCombatComponent::ApplyHitToActor(Player, Hit);
	switch (Result)
	{
	case EEMBHitResult::Parried:
		if (bReflectOnPerfectParry)
		{
			Reflect(Player);
		}
		else
		{
			ReturnToPool();
		}
		break;

	case EEMBHitResult::Dodged:
	case EEMBHitResult::Ignored:
		break; // le projectile traverse : esquive réussie ou cible morte

	default:
		ReturnToPool(); // Hit, Blocked, GuardBroken, Killed : le projectile est consommé
		break;
	}
}

void AEMBProjectile::ResolveHitOnReflectTarget(AActor* Target)
{
	FEMBHitData Hit = HitData; // réécrit par Reflect() : instigateur = le joueur, dégâts = ReflectDamage
	Hit.HitLocation = GetActorLocation();
	Hit.HitDirection = Movement->Velocity.GetSafeNormal();

	UEMBCombatComponent::ApplyHitToActor(Target, Hit);
	ReturnToPool();
}

void AEMBProjectile::Reflect(AActor* Parrier)
{
	AActor* Shooter = OriginBoss.Get();
	if (!Shooter)
	{
		ReturnToPool();
		return;
	}

	const FVector Here = GetActorLocation();
	const float Speed = ReflectSpeed > 0.f ? ReflectSpeed : CurrentSpeed;
	if (Speed <= KINDA_SMALL_NUMBER)
	{
		ReturnToPool();
		return;
	}

	// Visée avec anticipation : le boss continue de se déplacer pendant le trajet de retour.
	const float TravelDistance = static_cast<float>(FVector::Dist(Here, Shooter->GetActorLocation()));
	const FVector Predicted = Shooter->GetActorLocation() + Shooter->GetVelocity() * (TravelDistance / Speed);
	const FVector Direction = (Predicted - Here).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		ReturnToPool();
		return;
	}
	const FVector NewVelocity = Direction * Speed;

	ReflectTarget = Shooter;

	// Le coup renvoyé est celui du joueur : menace et crédit lui reviennent. Ni garde, ni parade possibles côté boss.
	HitData.Instigator = Parrier;
	HitData.Damage = ReflectDamage;
	HitData.PoiseDamage = 0.f;
	HitData.StaminaDamage = 0.f;
	HitData.CorruptionAmount = 0.f;
	HitData.DamageType = EEMBDamageType::Physical;
	HitData.HitBoneName = NAME_None;
	HitData.bUnblockable = true;
	HitData.bUnparryable = true;

	Collision->ClearMoveIgnoreActors();
	Collision->IgnoreActorWhenMoving(Parrier, true);

	// bReflected publié AVANT StartMotion : le projectile renvoyé ne peut plus toucher qu'ancien tireur, jamais un allié.
	PublishState(true, true, Here, NewVelocity);
	StartMotion(Here, NewVelocity);
	NotifyReflected();
	ArmLifetimeTimer(FMath::Max(MinReflectLifetime, TravelDistance / Speed + ReflectLifetimeMargin));
}

// ----------------------------------------------------------------------------------------------
// Réplication (clients)
// ----------------------------------------------------------------------------------------------

void AEMBProjectile::OnRep_NetState(const FEMBProjectileNetState& OldState)
{
	if (NetState.bActive)
	{
		const bool bNewMotion = !OldState.bActive || OldState.Sequence != NetState.Sequence;
		if (!bNewMotion)
		{
			return;
		}

		// Rattrapage de latence : le projectile est avancé du temps écoulé depuis le tir côté serveur (plafonné),
		// sinon il apparaîtrait en retard d'un ping complet (300 cm à 3 000 cm/s pour 100 ms).
		const double Elapsed = FMath::Clamp(GetSyncedTime() - NetState.ServerTime, 0.0, static_cast<double>(MaxClientCatchUpTime));
		StartMotion(NetState.Origin + NetState.Velocity * Elapsed, NetState.Velocity);

		if (NetState.bReflected)
		{
			NotifyReflected();
		}
		else
		{
			NotifyLaunched();
		}
	}
	else if (OldState.bActive)
	{
		StopMotion();
		NotifyEnded();
	}
}

// Mise en scène : ignorée sur serveur dédié (aucun rendu).
void AEMBProjectile::NotifyLaunched()
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnLaunched(NetState.Style);
	}
}

void AEMBProjectile::NotifyReflected()
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnReflected();
	}
}

void AEMBProjectile::NotifyEnded()
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnEnded();
	}
}
