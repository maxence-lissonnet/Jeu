// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBSeraphArena.h"

#include "Emberfall.h"
#include "EMBSeraphBoss.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

// ----------------------------------------------------------------------------------------------
// AEMBSeraphBell
// ----------------------------------------------------------------------------------------------

AEMBSeraphBell::AEMBSeraphBell()
{
	// Aucun Tick : la cloche réagit aux coups reçus.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetNetUpdateFrequency(5.f); // état quasi statique : peu de mises à jour réseau

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	// BlockAll (WorldStatic) : reçoit les balayages d'armes des joueurs et arrête les plumes de Nyxariel.
	Mesh->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	// BlockAll laisse les canaux personnalisés à leur défaut (Ignore) : sans cette ligne, les épées ne touchent pas la cloche.
	Mesh->SetCollisionResponseToChannel(EMBSeraphDefaults::MeleeHitChannel, ECR_Overlap);
}

void AEMBSeraphBell::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBSeraphBell, bUsed);
	DOREPLIFETIME(AEMBSeraphBell, bSilenced);
}

EEMBHitResult AEMBSeraphBell::ReceiveEMBHit(const FEMBHitData& Hit)
{
	const UWorld* World = GetWorld();
	if (!HasAuthority() || bSilenced || !World)
	{
		return EEMBHitResult::Ignored;
	}

	// Seul un joueur fait sonner la cloche (ni le boss, ni un projectile, ni un effet de zone).
	AActor* Striker = Hit.Instigator.Get();
	const APawn* StrikerPawn = Cast<APawn>(Striker);
	if (!StrikerPawn || !StrikerPawn->IsPlayerControlled())
	{
		return EEMBHitResult::Ignored;
	}

	// Un balayage d'arme peut rapporter la cloche plusieurs fois : une seule sonnerie par intervalle.
	const double Now = World->GetTimeSeconds();
	if (Now - LastRingTime < static_cast<double>(MinRingInterval))
	{
		return EEMBHitResult::Hit;
	}
	LastRingTime = Now;

	// L'arène décide de l'effet (Nyxariel vole-t-il ?) et appelle MarkUsed() si la cloche l'a étourdi.
	const bool bWasUsed = bUsed;
	OnBellStruck.Broadcast(this, Striker);
	Multicast_Ring(bUsed && !bWasUsed);
	return EEMBHitResult::Hit;
}

void AEMBSeraphBell::MarkUsed()
{
	if (!HasAuthority() || bUsed)
	{
		return;
	}
	bUsed = true; // répliqué : OnRep_Used chez les clients
	BP_OnUsedChanged(true);
}

void AEMBSeraphBell::SetSilenced(const bool bNewSilenced)
{
	if (!HasAuthority() || bSilenced == bNewSilenced)
	{
		return;
	}
	bSilenced = bNewSilenced;
	BP_OnSilenced(bSilenced);
}

void AEMBSeraphBell::OnRep_Used()
{
	BP_OnUsedChanged(bUsed);
}

void AEMBSeraphBell::OnRep_Silenced()
{
	BP_OnSilenced(bSilenced);
}

void AEMBSeraphBell::Multicast_Ring_Implementation(const bool bEffective)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnRung(bEffective);
	}
}

// ----------------------------------------------------------------------------------------------
// AEMBSeraphArena
// ----------------------------------------------------------------------------------------------

AEMBSeraphArena::AEMBSeraphArena()
{
	// Aucun Tick : l'état de l'arène est piloté par les délégués du boss et des cloches.
	PrimaryActorTick.bCanEverTick = false;

	// Non répliquée : chaque machine applique le même état à partir de la phase répliquée du boss.
	bReplicates = false;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AEMBSeraphArena::BeginPlay()
{
	Super::BeginPlay();

	for (AEMBSeraphBell* Bell : Bells)
	{
		if (Bell)
		{
			Bell->OnBellStruck.AddUniqueDynamic(this, &AEMBSeraphArena::HandleBellStruck);
		}
	}
	TryBindToBoss();
}

void AEMBSeraphArena::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BossSearchTimerHandle);
	}
	for (AEMBSeraphBell* Bell : Bells)
	{
		if (Bell)
		{
			Bell->OnBellStruck.RemoveDynamic(this, &AEMBSeraphArena::HandleBellStruck);
		}
	}
	if (AEMBSeraphBoss* Boss = BoundBoss.Get())
	{
		Boss->OnPhaseChanged.RemoveDynamic(this, &AEMBSeraphArena::HandleBossPhaseChanged);
		Boss->OnEncounterEnded.RemoveDynamic(this, &AEMBSeraphArena::HandleEncounterEnded);
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBSeraphArena::TryBindToBoss()
{
	AEMBSeraphBoss* Boss = BossActor.Get();
	if (!Boss)
	{
		Boss = Cast<AEMBSeraphBoss>(UGameplayStatics::GetActorOfClass(this, AEMBSeraphBoss::StaticClass()));
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
		GetWorldTimerManager().SetTimer(BossSearchTimerHandle, this, &AEMBSeraphArena::TryBindToBoss, EMBBossDefaults::ArenaBossSearchInterval, true);
	}
}

void AEMBSeraphArena::BindToBoss(AEMBSeraphBoss* Boss)
{
	if (!Boss || BoundBoss.Get() == Boss)
	{
		return;
	}

	if (AEMBSeraphBoss* Previous = BoundBoss.Get())
	{
		Previous->OnPhaseChanged.RemoveDynamic(this, &AEMBSeraphArena::HandleBossPhaseChanged);
		Previous->OnEncounterEnded.RemoveDynamic(this, &AEMBSeraphArena::HandleEncounterEnded);
	}

	BoundBoss = Boss;
	Boss->OnPhaseChanged.AddUniqueDynamic(this, &AEMBSeraphArena::HandleBossPhaseChanged);
	Boss->OnEncounterEnded.AddUniqueDynamic(this, &AEMBSeraphArena::HandleEncounterEnded);

	// Boss déjà vaincu (sauvegarde : ApplyDefeatedFromSave, ou reconnexion tardive) : état final calme directement.
	if (Boss->IsDead())
	{
		EnterFinalCalmState();
		return;
	}

	// Rattrapage : applique la phase courante (reconnexion en cours de combat).
	HandleBossPhaseChanged(Boss->GetCurrentPhase(), 0);
}

void AEMBSeraphArena::HandleBossPhaseChanged(const int32 NewPhase, const int32 OldPhase)
{
	// Idempotent : le délégué peut arriver deux fois (rattrapage + RepNotify). Après la défaite, l'arène reste calme.
	if (bFinalCalm || NewPhase <= AppliedPhase)
	{
		return;
	}
	AppliedPhase = NewPhase;
	OnArenaPhaseApplied(NewPhase);
}

void AEMBSeraphArena::HandleEncounterEnded()
{
	EnterFinalCalmState();
}

void AEMBSeraphArena::EnterFinalCalmState()
{
	if (bFinalCalm)
	{
		return;
	}
	bFinalCalm = true;

	for (AEMBSeraphBell* Bell : Bells)
	{
		if (Bell)
		{
			Bell->SetSilenced(true);
		}
	}
	OnArenaFinalCalmState();
}

void AEMBSeraphArena::HandleBellStruck(AEMBSeraphBell* Bell, AActor* Striker)
{
	// Décision serveur. Les cloches ne diffusent OnBellStruck que sur le serveur, mais on garde la garde par principe.
	AEMBSeraphBoss* Boss = BoundBoss.Get();
	if (!HasAuthority() || !Bell || !Boss || bFinalCalm || Bell->IsUsed())
	{
		return;
	}

	// Effet seulement si Nyxariel VOLE (phases 1-2) et peut être étourdi : au sol, en phase 3, étourdi, en transition ou mort, la cloche sonne
	// pour rien et reste utilisable.
	if (!Boss->ShouldFly() || !Boss->IsFlying() || Boss->IsStunned() || Boss->IsTransitioning() || Boss->IsDead())
	{
		return;
	}

	Boss->BeginStun(EMBSeraphDefaults::BellStunDuration);
	if (Boss->IsStunned())
	{
		Bell->MarkUsed(); // une fois par cloche
	}
}

int32 AEMBSeraphArena::GetUnusedBellCount() const
{
	int32 Count = 0;
	for (const AEMBSeraphBell* Bell : Bells)
	{
		if (Bell && !Bell->IsUsed())
		{
			++Count;
		}
	}
	return Count;
}
