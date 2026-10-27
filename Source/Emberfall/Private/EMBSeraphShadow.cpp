// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBSeraphShadow.h"

#include "EMBBossTags.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Dimensions de la capsule de requête (cm) : valeur de repli, ajustable dans le Blueprint. */
	constexpr float ShadowCapsuleRadius = 60.f;
	constexpr float ShadowCapsuleHalfHeight = 120.f;
}

AEMBSeraphShadow::AEMBSeraphShadow()
{
	// Tick actif seulement pendant un dash (SetActorTickEnabled) : au repos, l'ombre ne coûte rien.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	bReplicates = true;
	SetReplicateMovement(false); // voir l'en-tête : le dash est un aller-retour cosmétique
	SetNetUpdateFrequency(5.f);

	Collision = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Collision"));
	Collision->InitCapsuleSize(ShadowCapsuleRadius, ShadowCapsuleHalfHeight);
	SetRootComponent(Collision);

	// Sensible uniquement au balayage d'arme du joueur (EMBMeleeHit en Overlap : le multi-balayage la rapporte sans
	// s'arrêter dessus). Ni les projectiles, ni les pawns, ni la visibilité ne la voient.
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionObjectType(ECC_WorldDynamic);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(EMBSeraphDefaults::MeleeHitChannel, ECR_Overlap);
	Collision->SetGenerateOverlapEvents(false);
	Collision->CanCharacterStepUpOn = ECB_No;

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AEMBSeraphShadow::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBSeraphShadow, bDissipated);
}

void AEMBSeraphShadow::BeginPlay()
{
	Super::BeginPlay();

	HomeLocation = GetActorLocation();

	if (HasAuthority())
	{
		// Dissipation spontanée après 20 s (via Dissipate : fondu visuel, puis destruction).
		GetWorldTimerManager().SetTimer(LifetimeTimerHandle, this, &AEMBSeraphShadow::Dissipate, Lifetime, false);
	}
}

void AEMBSeraphShadow::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	Super::EndPlay(EndPlayReason);
}

AEMBSeraphBoss* AEMBSeraphShadow::GetBoss() const
{
	return Cast<AEMBSeraphBoss>(GetOwner());
}

// ----------------------------------------------------------------------------------------------
// Dissipation
// ----------------------------------------------------------------------------------------------

EEMBHitResult AEMBSeraphShadow::ReceiveEMBHit(const FEMBHitData& Hit)
{
	if (!HasAuthority() || bDissipated)
	{
		return EEMBHitResult::Ignored;
	}

	// Seul un joueur dissipe l'ombre (pas le boss, ni un projectile renvoyé, ni un effet de zone).
	const APawn* Striker = Cast<APawn>(Hit.Instigator.Get());
	if (!Striker || !Striker->IsPlayerControlled())
	{
		return EEMBHitResult::Ignored;
	}

	Dissipate();
	return EEMBHitResult::Killed;
}

void AEMBSeraphShadow::Dissipate()
{
	if (!HasAuthority() || bDissipated)
	{
		return;
	}

	bDissipated = true; // répliqué : OnRep_Dissipated chez les clients
	bDashing = false;
	DashHit = FEMBHitData();
	DashAlreadyHit.Reset();
	SetActorTickEnabled(false);
	SetActorEnableCollision(false);
	GetWorldTimerManager().ClearAllTimersForObject(this); // rejeux en attente et durée de vie

	BP_OnDissipated();
	SetLifeSpan(DissipateLingerTime);
}

void AEMBSeraphShadow::OnRep_Dissipated()
{
	if (bDissipated)
	{
		bDashing = false;
		SetActorTickEnabled(false);
		SetActorEnableCollision(false);
		BP_OnDissipated();
	}
}

// ----------------------------------------------------------------------------------------------
// Rejeu des attaques du boss
// ----------------------------------------------------------------------------------------------

void AEMBSeraphShadow::QueueEcho(const FEMBSeraphEcho& Echo)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || bDissipated || !World)
	{
		return;
	}

	// Un timer par événement (au plus quelques-uns en vol : les attaques durent plus de 0,3 s). Les poignées ne sont
	// pas conservées : ClearAllTimersForObject les annule à la dissipation.
	FTimerHandle Handle;
	World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(this, &AEMBSeraphShadow::ExecuteEcho, Echo),
		FMath::Max(EchoDelay, 0.02f), false);
}

void AEMBSeraphShadow::ExecuteEcho(FEMBSeraphEcho Echo)
{
	AEMBSeraphBoss* Boss = GetBoss();
	if (!HasAuthority() || bDissipated || !Boss || Boss->IsDead() || Boss->IsStunned() || Boss->IsTransitioning())
	{
		return; // le boss a été interrompu : son attaque n'existe plus, l'écho non plus
	}

	const FEMBBossAttackRow* Row = Boss->FindAttackRow(Echo.AttackTag);
	if (!Row)
	{
		return;
	}

	Multicast_PlayEcho(Echo.AttackTag, Echo.AimPoint);

	const FVector Here = GetActorLocation();
	const FGameplayTag Tag = Echo.AttackTag;

	if (Tag == EMBBossTags::Attack_Seraph_Thrust.GetTag() || Tag == EMBBossTags::Attack_Seraph_Feint.GetTag())
	{
		StartDash(*Row, Echo.AimPoint);
	}
	else if (Tag == EMBBossTags::Attack_Seraph_FeatherRain.GetTag())
	{
		Boss->LaunchFeatherFan(*Row, Here, Echo.AimPoint, DamageScale);
	}
	else if (Tag == EMBBossTags::Attack_Seraph_LanceThrow.GetTag())
	{
		Boss->LaunchLance(*Row, Here, Echo.AimPoint, DamageScale);
	}
	else if (Tag == EMBBossTags::Attack_Seraph_BlackHalo.GetTag())
	{
		const float Radius = Row->EffectRadius > 0.f ? Row->EffectRadius : Row->MaxRange;
		Boss->ApplyAreaDamage(Here, Radius, Boss->MakeSeraphHitData(*Row, DamageScale));
	}
	else if (Tag == EMBBossTags::Attack_Seraph_WingScissors.GetTag())
	{
		Boss->ApplyConeDamage(Here, Echo.AimPoint - Here, Row->HalfAngleDeg, Row->MaxRange,
			Boss->MakeSeraphHitData(*Row, DamageScale), false, true);
	}
	else if (Tag == EMBBossTags::Attack_Seraph_SpiralDive.GetTag() || Tag == EMBBossTags::Attack_Seraph_DeadStarFall.GetTag())
	{
		// Plongeons : l'ombre frappe le point que le boss vient de frapper (flash + impact), 0,3 s plus tard.
		// "Décaler sa roulade" : l'esquive du coup du boss ne protège pas du suivant.
		const float Radius = Row->EffectRadius > 0.f ? Row->EffectRadius : EMBSeraphDefaults::DiveImpactRadius;
		Boss->Multicast_ShowGroundMarker(Echo.AimPoint, Radius, EMBSeraphDefaults::DiveFlashDuration, true);
		Boss->ApplyAreaDamage(Echo.AimPoint, Radius, Boss->MakeSeraphHitData(*Row, DamageScale));
	}
}

void AEMBSeraphShadow::StartDash(const FEMBBossAttackRow& Row, const FVector& AimPoint)
{
	AEMBSeraphBoss* Boss = GetBoss();
	if (!Boss || bDashing)
	{
		return; // un seul dash à la fois ; un écho tombant pendant un dash est ignoré
	}

	// Même géométrie que le dash du boss : vers le point visé, pente bornée, 1 800 cm.
	const FVector Start = GetActorLocation();
	FVector Direction = (AimPoint - Start).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = GetActorForwardVector();
	}
	Direction.Z = FMath::Clamp<double>(Direction.Z, -EMBSeraphDefaults::DashMaxDescentSlope, EMBSeraphDefaults::DashMaxAscentSlope);
	Direction = Direction.GetSafeNormal();

	DashHit = Boss->MakeSeraphHitData(Row, DamageScale);
	DashAlreadyHit.Reset();
	DashContactRadius = Row.EffectRadius > 0.f ? Row.EffectRadius : EMBSeraphDefaults::DashContactRadius;

	const float Duration = Row.ActiveDuration > 0.f ? Row.ActiveDuration : EMBSeraphDefaults::DashDuration;
	Multicast_StartDash(Start, Start + Direction * EMBSeraphDefaults::DashDistance, Duration);
}

void AEMBSeraphShadow::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bDashing)
	{
		SetActorTickEnabled(false);
		return;
	}

	DashElapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(DashElapsed / FMath::Max(DashDuration, KINDA_SMALL_NUMBER), 0.f, 1.f);

	const FVector Previous = GetActorLocation();
	const FVector Next = FMath::Lerp(DashStart, DashEnd, Alpha);
	SetActorLocation(Next, false, nullptr, ETeleportType::None);

	// Dégâts : serveur uniquement, test sur le segment parcouru dans l'image (voir ResolveDashContact).
	if (HasAuthority())
	{
		if (AEMBSeraphBoss* Boss = GetBoss())
		{
			Boss->ResolveDashContact(Previous, Next, DashContactRadius, DashHit, DashAlreadyHit);
		}
	}

	if (Alpha >= 1.f)
	{
		FinishDash();
	}
}

void AEMBSeraphShadow::FinishDash()
{
	bDashing = false;
	DashHit = FEMBHitData(); // plus de gabarit de coup ni de joueurs mémorisés entre deux dashs
	DashAlreadyHit.Reset();
	SetActorLocation(HomeLocation, false, nullptr, ETeleportType::TeleportPhysics); // retour à la position d'origine
	SetActorTickEnabled(false);
}

// ----------------------------------------------------------------------------------------------
// Cosmétique
// ----------------------------------------------------------------------------------------------

void AEMBSeraphShadow::Multicast_PlayEcho_Implementation(const FGameplayTag AttackTag, const FVector AimPoint)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnEcho(AttackTag, AimPoint);
	}
}

void AEMBSeraphShadow::Multicast_StartDash_Implementation(const FVector Start, const FVector End, const float Duration)
{
	if (bDissipated)
	{
		return;
	}
	bDashing = true;
	DashStart = Start;
	DashEnd = End;
	DashDuration = FMath::Max(Duration, 0.05f);
	DashElapsed = 0.f;
	SetActorTickEnabled(true);
}
