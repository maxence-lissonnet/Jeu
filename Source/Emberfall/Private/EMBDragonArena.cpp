// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBDragonArena.h"

#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBCombatTypes.h"
#include "EMBDragonBoss.h"
#include "EMBDragonTypes.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

AEMBDragonArena::AEMBDragonArena()
{
	// Aucun Tick : l'état de l'arène est piloté par le délégué de phase, la lave par un timer.
	PrimaryActorTick.bCanEverTick = false;

	// Non répliquée : chaque machine applique le même état à partir de la phase répliquée du boss.
	bReplicates = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AEMBDragonArena::BeginPlay()
{
	Super::BeginPlay();
	RandomStream.GenerateNewSeed();
	TryBindToBoss();
}

void AEMBDragonArena::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(LavaTimerHandle);
		World->GetTimerManager().ClearTimer(BossSearchTimerHandle);
		World->GetTimerManager().ClearTimer(LightningTimerHandle);
		for (FTimerHandle& Handle : LightningImpactTimers)
		{
			World->GetTimerManager().ClearTimer(Handle);
		}
	}
	if (AEMBDragonBoss* Boss = BoundBoss.Get())
	{
		Boss->OnPhaseChanged.RemoveDynamic(this, &AEMBDragonArena::HandleBossPhaseChanged);
		Boss->OnEncounterEnded.RemoveDynamic(this, &AEMBDragonArena::HandleEncounterEnded);
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBDragonArena::TryBindToBoss()
{
	AEMBDragonBoss* Boss = BossActor.Get();
	if (!Boss)
	{
		Boss = Cast<AEMBDragonBoss>(UGameplayStatics::GetActorOfClass(this, AEMBDragonBoss::StaticClass()));
	}

	if (Boss)
	{
		GetWorldTimerManager().ClearTimer(BossSearchTimerHandle);
		BindToBoss(Boss);
		return;
	}

	// Sur un client, le boss répliqué peut arriver après l'arène : on réessaie à faible cadence jusqu'à son apparition.
	if (!GetWorldTimerManager().IsTimerActive(BossSearchTimerHandle))
	{
		GetWorldTimerManager().SetTimer(BossSearchTimerHandle, this, &AEMBDragonArena::TryBindToBoss, EMBBossDefaults::ArenaBossSearchInterval, true);
	}
}

void AEMBDragonArena::BindToBoss(AEMBDragonBoss* Boss)
{
	if (!Boss || BoundBoss.Get() == Boss)
	{
		return;
	}

	if (AEMBDragonBoss* Previous = BoundBoss.Get())
	{
		Previous->OnPhaseChanged.RemoveDynamic(this, &AEMBDragonArena::HandleBossPhaseChanged);
		Previous->OnEncounterEnded.RemoveDynamic(this, &AEMBDragonArena::HandleEncounterEnded);
	}

	BoundBoss = Boss;
	Boss->OnPhaseChanged.AddUniqueDynamic(this, &AEMBDragonArena::HandleBossPhaseChanged);
	Boss->OnEncounterEnded.AddUniqueDynamic(this, &AEMBDragonArena::HandleEncounterEnded);

	// Boss déjà vaincu (sauvegarde, reconnexion tardive) : état final calme directement, sans lave ni foudre.
	if (Boss->IsDead())
	{
		EnterFinalCalmState();
		return;
	}

	// Rattrapage : applique l'état initial (phase 1) et les phases déjà franchies (reconnexion en cours de combat).
	HandleBossPhaseChanged(Boss->GetCurrentPhase(), 0);
}

void AEMBDragonArena::HandleEncounterEnded()
{
	EnterFinalCalmState();
}

void AEMBDragonArena::EnterFinalCalmState()
{
	if (bFinalCalm)
	{
		return;
	}
	bFinalCalm = true;

	// Plus aucun danger : ni lave (zones oubliées, timer arrêté), ni foudre (timer et impacts en attente annulés).
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(LavaTimerHandle);
		TimerManager.ClearTimer(LightningTimerHandle);
		for (FTimerHandle& Handle : LightningImpactTimers)
		{
			TimerManager.ClearTimer(Handle);
		}
		LightningImpactTimers.Reset();
	}
	ActiveLavaZones.Reset();

	OnAshStormChanged(false);
	OnArenaFinalCalmState();
}

void AEMBDragonArena::HandleBossPhaseChanged(const int32 NewPhase, const int32 OldPhase)
{
	// Idempotent : le délégué peut arriver deux fois (rattrapage + RepNotify) ; une phase n'est appliquée qu'une fois.
	// Après la fin de la rencontre, aucune phase ne se (ré)applique : l'arène reste calme.
	if (bFinalCalm || NewPhase <= AppliedPhase)
	{
		return;
	}

	for (int32 Phase = AppliedPhase + 1; Phase <= NewPhase; ++Phase)
	{
		ApplyPhaseEntry(Phase);
	}
	AppliedPhase = NewPhase;

	UpdateLavaTimer();
	if (NewPhase >= 3)
	{
		StartLightning();
		OnAshStormChanged(true); // mise en scène seule, serveur et clients
	}
	OnArenaPhaseApplied(NewPhase);
}

void AEMBDragonArena::ApplyPhaseEntry(const int32 Phase)
{
	if (!PhaseConfigs.IsValidIndex(Phase - 1))
	{
		return;
	}
	const FEMBArenaPhaseConfig& Config = PhaseConfigs[Phase - 1];

	// Les soft pointers ne se résolvent que si l'acteur est chargé : les acteurs non chargés sont ignorés.
	for (const TSoftObjectPtr<AActor>& Soft : Config.ActorsToActivate)
	{
		ActivateArenaActor(Soft.Get());
	}
	for (const TSoftObjectPtr<AActor>& Soft : Config.LavaZonesToActivate)
	{
		if (AActor* Zone = Soft.Get())
		{
			ActivateArenaActor(Zone);
			RegisterLavaZone(Zone);
		}
	}
	for (const TSoftObjectPtr<AActor>& Soft : Config.LavaZonesToDeactivate)
	{
		if (AActor* Zone = Soft.Get())
		{
			UnregisterLavaZone(Zone);
			RemoveArenaActor(Zone);
		}
	}
	for (const TSoftObjectPtr<AActor>& Soft : Config.ActorsToDestroy)
	{
		RemoveArenaActor(Soft.Get());
	}
}

void AEMBDragonArena::ActivateArenaActor(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	Actor->SetActorHiddenInGame(false);
	Actor->SetActorEnableCollision(true);
	Actor->SetActorTickEnabled(true);
}

void AEMBDragonArena::RemoveArenaActor(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}

	// Réseau : le serveur détruit toujours (la destruction d'un acteur répliqué se propage aux clients). Un
	// acteur non répliqué (décor de niveau) doit être détruit localement par chaque machine. Un client qui voit
	// un acteur répliqué le masque et le désactive en attendant la destruction autoritaire.
	if (HasAuthority() || !Actor->GetIsReplicated())
	{
		Actor->Destroy();
	}
	else
	{
		Actor->SetActorHiddenInGame(true);
		Actor->SetActorEnableCollision(false);
		Actor->SetActorTickEnabled(false);
	}
}

void AEMBDragonArena::RegisterLavaZone(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}
	for (const FLavaZone& Zone : ActiveLavaZones)
	{
		if (Zone.Actor.Get() == Actor)
		{
			return;
		}
	}

	// Bornes mises en cache à l'activation (les zones de lave sont statiques) : le tick ne fait que des comparaisons.
	FLavaZone& NewZone = ActiveLavaZones.AddDefaulted_GetRef();
	NewZone.Actor = Actor;
	NewZone.Bounds = Actor->GetComponentsBoundingBox(true);
}

void AEMBDragonArena::UnregisterLavaZone(AActor* Actor)
{
	ActiveLavaZones.RemoveAll([Actor](const FLavaZone& Zone) { return Zone.Actor.Get() == Actor; });
}

void AEMBDragonArena::UpdateLavaTimer()
{
	// Les dégâts de lave sont autoritaires : jamais sur un client (GetNetMode() != NM_Client). Seul le serveur
	// (ou le jeu solo) arme le timer, et seulement s'il y a des zones actives.
	if (GetNetMode() == NM_Client)
	{
		return;
	}

	FTimerManager& TimerManager = GetWorldTimerManager();
	if (ActiveLavaZones.Num() > 0)
	{
		if (!TimerManager.IsTimerActive(LavaTimerHandle))
		{
			TimerManager.SetTimer(LavaTimerHandle, this, &AEMBDragonArena::TickLava, LavaTickInterval, true);
		}
	}
	else
	{
		TimerManager.ClearTimer(LavaTimerHandle);
	}
}

void AEMBDragonArena::TickLava()
{
	// Nettoie les zones dont l'acteur a été détruit.
	ActiveLavaZones.RemoveAll([](const FLavaZone& Zone) { return !Zone.Actor.IsValid(); });
	if (ActiveLavaZones.Num() == 0)
	{
		GetWorldTimerManager().ClearTimer(LavaTimerHandle);
		return;
	}

	TArray<APawn*> Players;
	AEMBDragonBoss::CollectAlivePlayerPawns(GetWorld(), Players);

	for (APawn* Pawn : Players)
	{
		const FVector Location = Pawn->GetActorLocation();
		for (const FLavaZone& Zone : ActiveLavaZones)
		{
			const bool bInside = Zone.Bounds.IsInsideXY(Location)
				&& Location.Z >= Zone.Bounds.Min.Z - LavaVerticalTolerance
				&& Location.Z <= Zone.Bounds.Max.Z + LavaVerticalTolerance;
			if (!bInside)
			{
				continue;
			}

			// Brûlure de lave : continue, ni parable ni blocable, sans dégâts de posture.
			FEMBHitData LavaHit;
			LavaHit.Instigator = this;
			LavaHit.Damage = LavaDamagePerSecond * LavaTickInterval;
			LavaHit.PoiseDamage = 0.f;
			LavaHit.DamageType = EEMBDamageType::Fire;
			LavaHit.HitLocation = Location;
			LavaHit.HitDirection = FVector::UpVector;
			LavaHit.bUnblockable = true;
			LavaHit.bUnparryable = true;
			LavaHit.bLethalZone = true; // v1.3 : la lave tue au "dernier point sûr" (le sac de mort n'est pas posé dans la lave)
			LavaHit.bDamageOverTime = true; // v1.1 : dégâts directs, ni esquive, ni counter-hit, ni multicast
			UEMBCombatComponent::ApplyHitToActor(Pawn, LavaHit);
			break; // un seul tick par joueur et par intervalle, même sur des zones qui se chevauchent
		}
	}
}

void AEMBDragonArena::StartLightning()
{
	// Serveur uniquement : les clients ne résolvent jamais de dégâts. Idempotent (HandleBossPhaseChanged l'est déjà).
	if (GetNetMode() == NM_Client)
	{
		return;
	}
	FTimerManager& TimerManager = GetWorldTimerManager();
	if (!TimerManager.IsTimerActive(LightningTimerHandle))
	{
		TimerManager.SetTimer(LightningTimerHandle, this, &AEMBDragonArena::StrikeLightning, EMBDragonDefaults::LightningInterval, true);
	}
}

void AEMBDragonArena::StrikeLightning()
{
	AEMBDragonBoss* Boss = BoundBoss.Get();
	UWorld* World = GetWorld();
	if (!Boss || !World || Boss->IsDead())
	{
		GetWorldTimerManager().ClearTimer(LightningTimerHandle);
		return;
	}

	TArray<APawn*> Players;
	AEMBDragonBoss::CollectAlivePlayerPawns(World, Players);
	if (Players.Num() == 0)
	{
		return;
	}

	// Joueur vivant au hasard, position décalée de 0 à 400 cm dans une direction aléatoire (à plat) :
	// l'éclair gêne sans suivre le joueur à la trace.
	const APawn* Victim = Players[RandomStream.RandRange(0, Players.Num() - 1)];
	const float OffsetDistance = RandomStream.FRandRange(0.f, EMBDragonDefaults::LightningMaxOffset);
	const float OffsetAngleRad = RandomStream.FRandRange(0.f, 2.f * PI);
	const FVector Offset(FMath::Cos(OffsetAngleRad) * OffsetDistance, FMath::Sin(OffsetAngleRad) * OffsetDistance, 0.f);
	const FVector Impact = Boss->ProjectToGround(Victim->GetActorLocation() + Offset);

	// Télégraphe de 1,5 s : marqueur au sol diffusé par le boss (Unreliable, cosmétique).
	Boss->Multicast_ShowGroundMarker(Impact, EMBDragonDefaults::LightningRadius, EMBDragonDefaults::LightningMarkerDuration, false);

	// Compactage : retire les handles dont le timer a expiré pour que le tableau ne grossisse pas pendant le combat.
	FTimerManager& TimerManager = World->GetTimerManager();
	LightningImpactTimers.RemoveAll([&TimerManager](const FTimerHandle& OldHandle) { return !TimerManager.IsTimerActive(OldHandle); });

	FTimerHandle& Handle = LightningImpactTimers.AddDefaulted_GetRef();
	TimerManager.SetTimer(Handle,
		FTimerDelegate::CreateUObject(this, &AEMBDragonArena::ResolveLightningImpact, Impact),
		EMBDragonDefaults::LightningMarkerDuration, false);
}

void AEMBDragonArena::ResolveLightningImpact(FVector Location)
{
	AEMBDragonBoss* Boss = BoundBoss.Get();
	if (!Boss || Boss->IsDead())
	{
		return; // boss disparu ou mort entre le marqueur et l'impact : pas de dégâts
	}

	// Flash d'impact pour les clients (même canal que le marqueur).
	Boss->Multicast_ShowGroundMarker(Location, EMBDragonDefaults::LightningRadius, EMBDragonDefaults::LightningImpactFlashDuration, true);

	TArray<APawn*> Players;
	AEMBDragonBoss::CollectAlivePlayerPawns(GetWorld(), Players);
	for (APawn* Pawn : Players)
	{
		const double Distance = FVector::Dist2D(Pawn->GetActorLocation(), Location);
		if (Distance > static_cast<double>(EMBDragonDefaults::LightningRadius + Pawn->GetSimpleCollisionRadius()))
		{
			continue;
		}

		// Coup normal : esquivable (i-frames), non parable, bDamageOverTime = false (v1.1).
		FEMBHitData Strike;
		Strike.Instigator = Boss; // le boss est l'instigateur (attribution de la mort, menace, journaux)
		Strike.Damage = EMBDragonDefaults::LightningDamage;
		Strike.PoiseDamage = 0.f;
		Strike.DamageType = EEMBDamageType::Lightning;
		Strike.HitLocation = Pawn->GetActorLocation();
		Strike.HitDirection = FVector::DownVector;
		Strike.bUnblockable = false;
		Strike.bUnparryable = true;
		Strike.bDamageOverTime = false;
		UEMBCombatComponent::ApplyHitToActor(Pawn, Strike);
	}
}
