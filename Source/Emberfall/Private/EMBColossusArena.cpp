// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBColossusArena.h"

#include "Emberfall.h"
#include "EMBColossusBoss.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// ----------------------------------------------------------------------------------------------
// AEMBObelisk
// ----------------------------------------------------------------------------------------------

AEMBObelisk::AEMBObelisk()
{
	// Aucun Tick : l'obélisque réagit aux coups reçus.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetNetUpdateFrequency(5.f); // état quasi statique : peu de mises à jour réseau

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	// BlockAll (WorldStatic) : arrête la charge du Colosse et reçoit les balayages d'armes des joueurs.
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	// Canal de coup de mêlée des joueurs (EMBMeleeHit = ECC_GameTraceChannel1) : BlockAll laisse les canaux personnalisés à
	// leur défaut (Ignore) et les épées traverseraient l'obélisque. Overlap suffit au balayage d'arme. Toute autre cible
	// d'arène que les joueurs doivent frapper doit recevoir la même réponse.
	Mesh->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);

	// PV de référence : EMBColossusDefaults::ObeliskMaxHealth (source unique), surchargeable par instance.
	MaxHealth = EMBColossusDefaults::ObeliskMaxHealth;
	Health = MaxHealth;
}

void AEMBObelisk::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBObelisk, Health);
	DOREPLIFETIME(AEMBObelisk, bLit);
}

void AEMBObelisk::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		Health = MaxHealth; // MaxHealth peut être surchargé par instance dans le niveau
	}
	if (Health <= 0.f)
	{
		ApplyDestroyedState();
	}
}

EEMBHitResult AEMBObelisk::ReceiveEMBHit(const FEMBHitData& Hit)
{
	if (!HasAuthority() || Health <= 0.f)
	{
		return EEMBHitResult::Ignored;
	}

	Health = FMath::Max(0.f, Health - FMath::Max(0.f, Hit.Damage));
	BP_OnHealthChanged(Health, MaxHealth);

	if (Health <= 0.f)
	{
		bLit = false;
		ApplyDestroyedState();
		return EEMBHitResult::Killed;
	}
	return EEMBHitResult::Hit;
}

void AEMBObelisk::SetLit(const bool bNewLit)
{
	if (!HasAuthority() || bLit == bNewLit || (bNewLit && Health <= 0.f))
	{
		return;
	}
	bLit = bNewLit;
	BP_OnLitChanged(bLit);
}

void AEMBObelisk::ApplyDestroyedState()
{
	if (bDestroyedStateApplied)
	{
		return;
	}
	bDestroyedStateApplied = true;

	// Détruit : il ne bloque plus la charge ni les coups. Le Blueprint masque le mesh / joue l'éboulement.
	SetActorEnableCollision(false);
	BP_OnDestroyed();
	OnObeliskDestroyed.Broadcast(this);
}

void AEMBObelisk::OnRep_Health(const float OldHealth)
{
	BP_OnHealthChanged(Health, MaxHealth);
	if (Health <= 0.f)
	{
		ApplyDestroyedState();
	}
}

void AEMBObelisk::OnRep_Lit()
{
	BP_OnLitChanged(bLit);
}

// ----------------------------------------------------------------------------------------------
// AEMBColossusArena
// ----------------------------------------------------------------------------------------------

AEMBColossusArena::AEMBColossusArena()
{
	// Aucun Tick : l'état de l'arène est piloté par les délégués du boss.
	PrimaryActorTick.bCanEverTick = false;

	// Non répliquée : chaque machine applique le même état à partir de la phase répliquée du boss.
	bReplicates = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AEMBColossusArena::BeginPlay()
{
	Super::BeginPlay();
	TryBindToBoss();
}

void AEMBColossusArena::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BossSearchTimerHandle);
	}
	if (AEMBColossusBoss* Boss = BoundBoss.Get())
	{
		Boss->OnPhaseChanged.RemoveDynamic(this, &AEMBColossusArena::HandleBossPhaseChanged);
		Boss->OnEncounterEnded.RemoveDynamic(this, &AEMBColossusArena::HandleEncounterEnded);
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBColossusArena::TryBindToBoss()
{
	AEMBColossusBoss* Boss = BossActor.Get();
	if (!Boss)
	{
		Boss = Cast<AEMBColossusBoss>(UGameplayStatics::GetActorOfClass(this, AEMBColossusBoss::StaticClass()));
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
		GetWorldTimerManager().SetTimer(BossSearchTimerHandle, this, &AEMBColossusArena::TryBindToBoss, EMBBossDefaults::ArenaBossSearchInterval, true);
	}
}

void AEMBColossusArena::BindToBoss(AEMBColossusBoss* Boss)
{
	if (!Boss || BoundBoss.Get() == Boss)
	{
		return;
	}

	if (AEMBColossusBoss* Previous = BoundBoss.Get())
	{
		Previous->OnPhaseChanged.RemoveDynamic(this, &AEMBColossusArena::HandleBossPhaseChanged);
		Previous->OnEncounterEnded.RemoveDynamic(this, &AEMBColossusArena::HandleEncounterEnded);
	}

	BoundBoss = Boss;
	Boss->SetArena(this);
	Boss->OnPhaseChanged.AddUniqueDynamic(this, &AEMBColossusArena::HandleBossPhaseChanged);
	Boss->OnEncounterEnded.AddUniqueDynamic(this, &AEMBColossusArena::HandleEncounterEnded);

	// Boss déjà vaincu (sauvegarde, reconnexion tardive) : état final calme directement.
	if (Boss->IsDead())
	{
		EnterFinalCalmState();
		return;
	}

	// Rattrapage : applique la phase courante (reconnexion en cours de combat).
	HandleBossPhaseChanged(Boss->GetCurrentPhase(), 0);
}

void AEMBColossusArena::HandleBossPhaseChanged(const int32 NewPhase, const int32 OldPhase)
{
	// Idempotent : le délégué peut arriver deux fois (rattrapage + RepNotify). Après la défaite, l'arène reste calme.
	if (bFinalCalm || NewPhase <= AppliedPhase)
	{
		return;
	}
	AppliedPhase = NewPhase;

	// Phase 2 : les runes s'embrasent, les obélisques intacts s'allument (serveur : état répliqué aux clients).
	if (NewPhase >= 2)
	{
		for (AEMBObelisk* Obelisk : Obelisks)
		{
			if (Obelisk && Obelisk->IsIntact())
			{
				Obelisk->SetLit(true);
			}
		}
	}
	OnArenaPhaseApplied(NewPhase);
}

void AEMBColossusArena::HandleEncounterEnded()
{
	EnterFinalCalmState();
}

void AEMBColossusArena::EnterFinalCalmState()
{
	if (bFinalCalm)
	{
		return;
	}
	bFinalCalm = true;

	for (AEMBObelisk* Obelisk : Obelisks)
	{
		if (Obelisk)
		{
			Obelisk->SetLit(false);
		}
	}
	OnArenaFinalCalmState();
}

int32 AEMBColossusArena::GetIntactObeliskCount() const
{
	int32 Count = 0;
	for (const AEMBObelisk* Obelisk : Obelisks)
	{
		if (Obelisk && Obelisk->IsIntact())
		{
			++Count;
		}
	}
	return Count;
}

bool AEMBColossusArena::IsChargeObstacle(const AActor* HitActor) const
{
	if (!HitActor || HitActor->IsA<APawn>())
	{
		return false; // un joueur ou un PNJ n'arrête pas la charge : il est simplement touché
	}
	if (HitActor->IsA<AEMBObelisk>() || HitActor->ActorHasTag(ChargeWallTag))
	{
		return true;
	}
	return bAnyStaticActorStopsCharge;
}
