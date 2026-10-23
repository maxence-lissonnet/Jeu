// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBDragonBoss.h"

#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBDragonAIController.h"
#include "EMBGameplayTags.h"
#include "EMBProgressionSettings.h"
#include "EMBProgressionSubsystem.h"
#include "EMBStatsComponent.h"

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "BrainComponent.h"
#include "CollisionQueryParams.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/DataTable.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Nombre maximal de parents remontés depuis l'os touché pour trouver un point faible. */
	constexpr int32 MaxBoneClimbDepth = 16;

	/** Demi-vie de la menace (s) et dégâts équivalant à 1,0 point de score de menace. */
	constexpr float ThreatHalfLife = 8.f;
	constexpr float ThreatDamageForOneScorePoint = 2000.f;

	/** Durée de référence du repoussement (rafale d'ailes) : la vitesse de lancement en est déduite. */
	constexpr float KnockbackTravelTime = 0.45f;

	/** Marge entre le verrouillage de l'orientation et le premier impact (le joueur voit la direction définitive). */
	constexpr float FacingLockLeadTime = 0.35f;

	/** Le dragon n'est pas un Pawn à suivre en continu : tick des zones de feu (s). */
	constexpr float FireZoneTickInterval = 0.25f;
}

AEMBDragonBoss::AEMBDragonBoss()
{
	// Aucun Tick : phases, recharges, brûlures et zones de feu fonctionnent par événements et timers.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	bAlwaysRelevant = true; // le boss final reste pertinent pour tous les joueurs de l'arène
	SetNetUpdateFrequency(30.f);

	AIControllerClass = AEMBDragonAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	StatsComponent = CreateDefaultSubobject<UEMBStatsComponent>(TEXT("StatsComponent"));
	StatsComponent->SetIsReplicated(true);
	ConfigureStatsDefaults();

	// Un dragon titanesque pivote lentement : son orientation est une information de lecture pour le joueur.
	bUseControllerRotationYaw = false;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->bOrientRotationToMovement = false;
		Move->bUseControllerDesiredRotation = true;
		Move->RotationRate = FRotator(0.f, 90.f, 0.f);
		Move->MaxWalkSpeed = EMBDragonDefaults::WalkSpeed;
		Move->MaxFlySpeed = EMBDragonDefaults::FlySpeed;
		Move->BrakingDecelerationFlying = 1000000.f; // le vol est scripté (StepFlightToward) : pas d'inertie propre
	}

	// Réseau / optimisation : sur serveur dédié, l'animation doit tourner pour que les os (points faibles) et
	// les notifies de montage soient valides, même si le mesh n'est jamais rendu.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	}

	// Points faibles par défaut (noms d'os à adapter au squelette final, table éditable dans le Blueprint).
	WeakPointMultipliers.Add(TEXT("head"), EMBDragonDefaults::WeakPointHead);
	WeakPointMultipliers.Add(TEXT("belly"), EMBDragonDefaults::WeakPointBelly);
	WeakPointMultipliers.Add(TEXT("wing_l"), EMBDragonDefaults::WeakPointWings);
	WeakPointMultipliers.Add(TEXT("wing_r"), EMBDragonDefaults::WeakPointWings);
	WeakPointMultipliers.Add(TEXT("tail_01"), EMBDragonDefaults::WeakPointTail);
}

void AEMBDragonBoss::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Peu de propriétés, toutes à faible fréquence de changement : coût réseau négligeable.
	// Les PV et la posture sont répliqués par UEMBStatsComponent.
	DOREPLIFETIME(AEMBDragonBoss, CurrentPhase);
	DOREPLIFETIME(AEMBDragonBoss, bIsTransitioning);
	DOREPLIFETIME(AEMBDragonBoss, bIsStunned);
	DOREPLIFETIME(AEMBDragonBoss, bIsDead);
	DOREPLIFETIME(AEMBDragonBoss, bIsFlying);
	DOREPLIFETIME(AEMBDragonBoss, bIsVulnerable);
}

void AEMBDragonBoss::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if (StatsComponent)
	{
		// Les délégués ne sont déclenchés que sur le serveur (ApplyDamage / ApplyPoiseDamage sont autoritaires).
		StatsComponent->OnDeath.AddUniqueDynamic(this, &AEMBDragonBoss::HandleStatsDeath);
		StatsComponent->OnPoiseBroken.AddUniqueDynamic(this, &AEMBDragonBoss::HandlePoiseBroken);
	}
}

void AEMBDragonBoss::BeginPlay()
{
	Super::BeginPlay();

	RandomStream.GenerateNewSeed();
	BuildAttackCache();
	PreloadAttackAssets();
	RefreshStateTags();

	if (HasAuthority())
	{
		// PV solo par défaut ; recalculés selon le nombre de joueurs au début du combat (BeginEncounter).
		ScaleHealthForPlayerCount(1);

		// Monde chargé d'une sauvegarde où le dragon est déjà vaincu : il ne combat pas. Si la sauvegarde restaure
		// les événements monde APRÈS ce BeginPlay (chargement asynchrone), le système de sauvegarde doit appeler
		// ApplyDefeatedFromSave() ; les deux chemins sont idempotents.
		CheckDefeatedFromSave();
	}
}

void AEMBDragonBoss::CheckDefeatedFromSave()
{
	const UWorld* World = GetWorld();
	const UEMBProgressionSubsystem* Progression = World ? World->GetSubsystem<UEMBProgressionSubsystem>() : nullptr;
	if (Progression && Progression->IsWorldEventRecorded(EMBTags::Event_Boss_Killed_Dragon.GetTag()))
	{
		ApplyDefeatedFromSave();
	}
}

bool AEMBDragonBoss::IsEncounterActive() const
{
	if (!bEncounterStarted || bIsDead)
	{
		return false;
	}
	// CountPlayersWithin ne compte que les joueurs vivants (CollectAlivePlayerPawns).
	return CountPlayersWithin(UEMBProgressionSettings::Get()->ParticipantRadius) > 0;
}

void AEMBDragonBoss::ApplyDefeatedFromSave()
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	// Choix : le dragon est mis en sommeil et NON détruit. Un acteur placé dans le niveau est référencé par
	// l'arène, les Blueprints et la séquence de mise en scène ; le détruire laisserait des références nulles et
	// compliquerait la réplication pour un joueur qui rejoint plus tard. Dormant, il ne coûte rien : pas de Tick,
	// pas d'IA, pas de collision, caché (bHidden et la collision sont répliqués, les clients suivent).
	bEncounterStarted = true; // aucun combat ne peut plus démarrer (BeginEncounter est idempotent)
	bIsDead = true;           // répliqué : OnRep_IsDead côté clients
	bIsStunned = false;
	bIsTransitioning = false;
	bIsVulnerable = false;

	GetWorldTimerManager().ClearAllTimersForObject(this);
	AttackTimers.Reset();
	BurnEndTimes.Reset();
	FireZones.Reset();

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	if (AController* DragonController = GetController())
	{
		if (UBrainComponent* Brain = DragonController->FindComponentByClass<UBrainComponent>())
		{
			Brain->StopLogic(TEXT("DefeatedFromSave"));
		}
	}

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);

	RefreshStateTags();
	PushStateToBlackboard();

	// Ni XP, ni OnBossDefeated : l'événement est déjà enregistré. L'arène passe en état final calme.
	OnEncounterEnded.Broadcast();
}

void AEMBDragonBoss::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
	}
	PreloadHandle.Reset();

	Super::EndPlay(EndPlayReason);
}

// ----------------------------------------------------------------------------------------------
// Configuration
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::ConfigureStatsDefaults()
{
	// Constructeur (CDO) : valeurs par défaut du composant, ensuite surchargeables dans le Blueprint.
	if (StatsComponent)
	{
		StatsComponent->MaxPoise = EMBDragonDefaults::MaxPoise;
		StatsComponent->PoiseRegenRate = EMBDragonDefaults::PoiseRegenRate;
		StatsComponent->PoiseRegenDelay = EMBDragonDefaults::PoiseRegenDelay;
	}
}

void AEMBDragonBoss::ScaleHealthForPlayerCount(const int32 PlayerCount)
{
	if (!StatsComponent)
	{
		return;
	}

	const int32 Players = FMath::Clamp(PlayerCount, 1, EMBDragonDefaults::MaxCoopPlayers);
	const float ScaledMax = EMBDragonDefaults::MaxHealthSolo
		* (1.f + EMBDragonDefaults::CoopHealthBonusPerExtraPlayer * static_cast<float>(Players - 1));
	StatsComponent->SetMaxHealth(ScaledMax, true);
}

void AEMBDragonBoss::BeginEncounter()
{
	if (!HasAuthority() || bEncounterStarted || bIsDead)
	{
		return;
	}
	// Mise à l'échelle figée une fois pour toutes (premier joueur détecté, cible choisie ou premier dégât) :
	// un joueur qui rejoint après ce point ne modifie plus les PV du boss.
	bEncounterStarted = true;

	TArray<APawn*> Players;
	CollectAlivePlayerPawns(GetWorld(), Players);
	ScaleHealthForPlayerCount(Players.Num());

	UE_LOG(LogEmberfall, Log, TEXT("Vaelthyrax : combat engage avec %d joueur(s), PV max %.0f."),
		Players.Num(), StatsComponent ? StatsComponent->GetMaxHealth() : 0.f);
}

void AEMBDragonBoss::BuildAttackCache()
{
	AttackRows.Reset();
	AttackRowByTag.Reset();

	if (!AttackTable)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBDragonBoss %s : AttackTable non assignee, aucune attaque disponible."), *GetName());
		return;
	}

	TArray<FEMBDragonAttackRow*> Rows;
	AttackTable->GetAllRows<FEMBDragonAttackRow>(TEXT("AEMBDragonBoss::BuildAttackCache"), Rows);

	for (const FEMBDragonAttackRow* Row : Rows)
	{
		if (Row && Row->AttackTag.IsValid())
		{
			// Les pointeurs restent valides tant que la DataTable (référencée par UPROPERTY) vit.
			AttackRows.Add(Row);
			AttackRowByTag.Add(Row->AttackTag, Row);
		}
	}
}

void AEMBDragonBoss::PreloadAttackAssets()
{
	// Les montages et VFX sont des soft pointers : on les charge de façon asynchrone dès le BeginPlay pour
	// éviter un chargement synchrone (hitch) au premier souffle, sur le serveur comme sur les clients.
	TArray<FSoftObjectPath> Paths;
	for (const FEMBDragonAttackRow* Row : AttackRows)
	{
		if (!Row->Montage.IsNull())
		{
			Paths.AddUnique(Row->Montage.ToSoftObjectPath());
		}
		if (!Row->VFX.IsNull())
		{
			Paths.AddUnique(Row->VFX.ToSoftObjectPath());
		}
	}

	if (Paths.Num() > 0)
	{
		PreloadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths);
	}
}

const FEMBDragonAttackRow* AEMBDragonBoss::FindAttackRow(const FGameplayTag& AttackTag) const
{
	const FEMBDragonAttackRow* const* Found = AttackRowByTag.Find(AttackTag);
	return Found ? *Found : nullptr;
}

UNiagaraSystem* AEMBDragonBoss::ResolveVFX(const TSoftObjectPtr<UNiagaraSystem>& Soft) const
{
	if (Soft.IsNull())
	{
		return nullptr;
	}
	UNiagaraSystem* System = Soft.Get();
	if (!System)
	{
		// Repli : le préchargement n'est pas terminé. Chargement synchrone (hitch possible, à éviter via le préchargement).
		System = Soft.LoadSynchronous();
	}
	return System;
}

UAnimMontage* AEMBDragonBoss::ResolveMontage(const TSoftObjectPtr<UAnimMontage>& Soft) const
{
	if (Soft.IsNull())
	{
		return nullptr;
	}
	UAnimMontage* Montage = Soft.Get();
	if (!Montage)
	{
		Montage = Soft.LoadSynchronous();
	}
	return Montage;
}

// ----------------------------------------------------------------------------------------------
// Tags et état
// ----------------------------------------------------------------------------------------------

FGameplayTag AEMBDragonBoss::GetPhaseTag(const int32 Phase)
{
	switch (Phase)
	{
	case 1: return EMBTags::Boss_Phase_1.GetTag();
	case 2: return EMBTags::Boss_Phase_2.GetTag();
	default: return EMBTags::Boss_Phase_3.GetTag();
	}
}

void AEMBDragonBoss::RefreshStateTags()
{
	StateTags.Reset();
	StateTags.AddTag(GetPhaseTag(CurrentPhase));
	if (bIsTransitioning)
	{
		StateTags.AddTag(EMBTags::State_Combat_Invulnerable.GetTag());
	}
	if (bIsStunned)
	{
		StateTags.AddTag(EMBTags::State_Combat_Staggered.GetTag());
	}
	if (bIsDead)
	{
		StateTags.AddTag(EMBTags::State_Dead.GetTag());
	}
}

void AEMBDragonBoss::GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const
{
	TagContainer.AppendTags(StateTags);
}

void AEMBDragonBoss::PushStateToBlackboard()
{
	// Les états critiques sont poussés immédiatement (et non à l'intervalle du service) pour que le
	// Behavior Tree abandonne ses tâches dès le changement d'état (observer aborts).
	if (AEMBDragonAIController* DragonController = Cast<AEMBDragonAIController>(GetController()))
	{
		DragonController->SyncBlackboardState();
	}
}

float AEMBDragonBoss::GetAttackPlayRate() const
{
	return IsEnraged() ? EMBDragonDefaults::EnragedPlayRate : 1.f;
}

float AEMBDragonBoss::GetTelegraphMultiplier() const
{
	return IsEnraged() ? EMBDragonDefaults::EnragedTelegraphMultiplier : 1.f;
}

float AEMBDragonBoss::GetCooldownMultiplier() const
{
	return IsEnraged() ? EMBDragonDefaults::EnragedCooldownMultiplier : 1.f;
}

// ----------------------------------------------------------------------------------------------
// Réception des coups
// ----------------------------------------------------------------------------------------------

float AEMBDragonBoss::ResolveWeakPointMultiplier(const FName BoneName) const
{
	if (BoneName.IsNone())
	{
		return 1.f;
	}

	const USkeletalMeshComponent* MeshComp = GetMesh();
	FName Current = BoneName;
	for (int32 Depth = 0; Depth < MaxBoneClimbDepth && !Current.IsNone(); ++Depth)
	{
		if (const float* Multiplier = WeakPointMultipliers.Find(Current))
		{
			return *Multiplier;
		}
		// Un coup sur "tail_05" hérite du multiplicateur de "tail_01" si l'os est un descendant listé.
		Current = MeshComp ? MeshComp->GetParentBone(Current) : NAME_None;
	}
	return 1.f;
}

EEMBHitResult AEMBDragonBoss::ReceiveEMBHit(const FEMBHitData& Hit)
{
	// Les coups ne sont résolus que par le serveur (ApplyHitToActor est appelé côté serveur).
	if (!HasAuthority() || bIsDead || !StatsComponent)
	{
		return EEMBHitResult::Ignored;
	}

	// Invulnérabilité pendant le rugissement de transition de phase.
	if (bIsTransitioning)
	{
		return EEMBHitResult::Ignored;
	}

	// Le combat commence au plus tard au premier dégât : la mise à l'échelle coop est figée ici si elle ne l'a
	// pas déjà été (détection, ou cible choisie par le service).
	BeginEncounter();

	const bool bWasStunned = bIsStunned;

	float Multiplier = ResolveWeakPointMultiplier(Hit.HitBoneName);
	if (bWasStunned)
	{
		// Riposte sur boss à la posture brisée : coup critique.
		Multiplier *= EMBDragonDefaults::CriticalHitMultiplier;
	}
	if (bIsVulnerable)
	{
		Multiplier *= VulnerableDamageMultiplier;
	}

	const float FinalDamage = Hit.Damage * Multiplier;
	if (FinalDamage > 0.f)
	{
		StatsComponent->ApplyDamage(FinalDamage, Hit.Instigator);
		AddThreat(Hit.Instigator, FinalDamage);
	}

	if (StatsComponent->IsDead())
	{
		return EEMBHitResult::Killed; // HandleStatsDeath a déjà été déclenché par OnDeath
	}

	// Posture : ignorée pendant l'étourdissement. La rupture déclenche HandlePoiseBroken via OnPoiseBroken.
	if (!bWasStunned && Hit.PoiseDamage > 0.f)
	{
		StatsComponent->ApplyPoiseDamage(Hit.PoiseDamage, Hit.Instigator);
	}

	// Une seule transition par seuil, même si ce coup a "sauté" un seuil (voir EvaluatePhaseThresholds).
	EvaluatePhaseThresholds();

	return EEMBHitResult::Hit;
}

void AEMBDragonBoss::HandlePoiseBroken(AActor* InInstigator)
{
	if (!HasAuthority() || bIsDead || bIsTransitioning || bIsStunned)
	{
		return;
	}

	// Interrompt l'attaque en cours : les tâches du BT sont abandonnées via bIsStunned (observer aborts).
	CancelTimedAttack(false);
	if (bIsFlying)
	{
		SetFlying(false);
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}

	bIsStunned = true;
	RefreshStateTags();
	OnStunChanged.Broadcast(true);

	// Arrêt inconditionnel des montages (attaque en cours, y compris celle d'une tâche BT), puis montage d'étourdissement.
	Multicast_StopMontages(0.1f);
	if (StunMontage)
	{
		Multicast_PlayMontage(StunMontage, 1.f, NAME_None);
	}

	GetWorldTimerManager().SetTimer(StunTimerHandle, this, &AEMBDragonBoss::EndStun,
		EMBDragonDefaults::PostureBreakStunDuration, false);

	PushStateToBlackboard();
}

void AEMBDragonBoss::EndStun()
{
	if (!bIsStunned)
	{
		return;
	}
	bIsStunned = false;
	RefreshStateTags();
	OnStunChanged.Broadcast(false);
	PushStateToBlackboard();
}

void AEMBDragonBoss::BeginVulnerableWindow(const float Duration)
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}
	bIsVulnerable = true;
	GetWorldTimerManager().SetTimer(VulnerableTimerHandle, this, &AEMBDragonBoss::EndVulnerableWindow, FMath::Max(Duration, 0.05f), false);
	PushStateToBlackboard();
}

void AEMBDragonBoss::EndVulnerableWindow()
{
	bIsVulnerable = false;
	PushStateToBlackboard();
}

// ----------------------------------------------------------------------------------------------
// Phases
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::EvaluatePhaseThresholds()
{
	if (!HasAuthority() || bIsDead || bIsTransitioning || !StatsComponent)
	{
		return;
	}

	const float HealthPercent = StatsComponent->GetHealthPercent();

	// Les phases sont franchies une par une : un gros coup qui saute le seuil 70 % ET 35 % déclenche P2,
	// puis (après le rugissement) EvaluatePhaseThresholds est rappelé et déclenche P3. Aucun seuil n'est
	// traité deux fois car CurrentPhase ne fait qu'augmenter.
	if (CurrentPhase < 2 && HealthPercent <= EMBDragonDefaults::Phase2HealthThreshold)
	{
		StartPhaseTransition(2);
	}
	else if (CurrentPhase == 2 && HealthPercent <= EMBDragonDefaults::Phase3HealthThreshold)
	{
		StartPhaseTransition(3);
	}
}

void AEMBDragonBoss::StartPhaseTransition(const int32 TargetPhase)
{
	if (!HasAuthority() || bIsTransitioning || bIsDead)
	{
		return;
	}

	// Un étourdissement en cours est annulé : la transition prime.
	if (bIsStunned)
	{
		GetWorldTimerManager().ClearTimer(StunTimerHandle);
		bIsStunned = false;
		OnStunChanged.Broadcast(false);
	}

	CancelTimedAttack(false);
	SetFacingLocked(false);
	if (bIsFlying)
	{
		SetFlying(false);
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}

	PendingPhase = TargetPhase;
	bIsTransitioning = true; // invulnérabilité (voir ReceiveEMBHit)
	RefreshStateTags();

	if (PhaseTransitionMontage)
	{
		// Le montage de rugissement est étiré pour couvrir exactement la durée d'invulnérabilité.
		const float Rate = PhaseTransitionMontage->GetPlayLength() / EMBDragonDefaults::PhaseTransitionDuration;
		Multicast_StopMontages(0.1f);
		Multicast_PlayMontage(PhaseTransitionMontage, FMath::Max(Rate, 0.1f), NAME_None);
	}

	OnPhaseTransitionStarted.Broadcast(TargetPhase);
	PushStateToBlackboard();

	GetWorldTimerManager().SetTimer(PhaseTransitionTimerHandle, this, &AEMBDragonBoss::FinishPhaseTransition,
		EMBDragonDefaults::PhaseTransitionDuration, false);
}

void AEMBDragonBoss::FinishPhaseTransition()
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	const int32 OldPhase = CurrentPhase;
	CurrentPhase = PendingPhase; // répliqué : les clients reçoivent OnRep_CurrentPhase
	bIsTransitioning = false;

	if (CurrentPhase >= 3)
	{
		NextCataclysmTime = GetWorld()->GetTimeSeconds() + EMBDragonDefaults::CataclysmFirstDelayAfterPhase3;
	}

	RefreshStateTags();
	OnPhaseChanged.Broadcast(CurrentPhase, OldPhase); // serveur : le changement d'arène démarre ici
	PushStateToBlackboard();

	// Les PV peuvent déjà être sous le seuil suivant (gros dégâts pendant la phase précédente).
	EvaluatePhaseThresholds();
}

void AEMBDragonBoss::OnRep_CurrentPhase(const int32 OldPhase)
{
	RefreshStateTags();
	OnPhaseChanged.Broadcast(CurrentPhase, OldPhase);
}

void AEMBDragonBoss::OnRep_IsTransitioning()
{
	RefreshStateTags();
	if (bIsTransitioning)
	{
		// Côté client la phase cible n'est pas répliquée : c'est toujours la phase suivante.
		OnPhaseTransitionStarted.Broadcast(CurrentPhase + 1);
	}
}

void AEMBDragonBoss::OnRep_IsStunned()
{
	RefreshStateTags();
	OnStunChanged.Broadcast(bIsStunned);
}

void AEMBDragonBoss::OnRep_IsDead()
{
	RefreshStateTags();
	if (bIsDead)
	{
		OnEncounterEnded.Broadcast(); // clients : l'arène passe en état final calme
	}
}

bool AEMBDragonBoss::IsCataclysmDue() const
{
	if (!HasAuthority() || bIsDead || bIsTransitioning || CurrentPhase < 3 || NextCataclysmTime < 0.0)
	{
		return false;
	}
	return GetWorld() && GetWorld()->GetTimeSeconds() >= NextCataclysmTime;
}

void AEMBDragonBoss::NotifyCataclysmFired()
{
	if (GetWorld())
	{
		NextCataclysmTime = GetWorld()->GetTimeSeconds() + EMBDragonDefaults::CataclysmInterval;
	}
}

// ----------------------------------------------------------------------------------------------
// Mort
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::HandleStatsDeath(AActor* Killer)
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	const bool bWasStunned = bIsStunned;
	bIsDead = true;
	bIsStunned = false;
	bIsTransitioning = false;
	bIsVulnerable = false;
	if (bWasStunned)
	{
		OnStunChanged.Broadcast(false); // l'écoute (UI, mise en scène) ne doit pas rester sur "étourdi"
	}

	FTimerManager& TimerManager = GetWorldTimerManager();
	TimerManager.ClearTimer(PhaseTransitionTimerHandle);
	TimerManager.ClearTimer(StunTimerHandle);
	TimerManager.ClearTimer(VulnerableTimerHandle);
	TimerManager.ClearTimer(BurnTimerHandle);
	TimerManager.ClearTimer(FireZoneTimerHandle);
	BurnEndTimes.Reset();
	FireZones.Reset();

	CancelTimedAttack(false);
	SetFlying(false);
	Multicast_StopBreathVFX();

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}

	if (DeathMontage)
	{
		Multicast_StopMontages(0.1f);
		Multicast_PlayMontage(DeathMontage, 1.f, NAME_None);
	}

	if (AController* DragonController = GetController())
	{
		if (UBrainComponent* Brain = DragonController->FindComponentByClass<UBrainComponent>())
		{
			Brain->StopLogic(TEXT("BossDead"));
		}
	}

	RefreshStateTags();
	PushStateToBlackboard();

	// Diffusion de l'événement de progression (Event.Boss.Killed.Dragon) : autoritaire, serveur uniquement.
	OnBossDefeated.Broadcast(EMBTags::Event_Boss_Killed_Dragon.GetTag(), Killer);

	// Progression : enregistre l'événement monde et distribue l'XP aux participants (joueurs vivants ou morts
	// dans le rayon, cf. §7.2). Le sous-système est autoritaire et ignore un événement déjà enregistré.
	if (UWorld* World = GetWorld())
	{
		if (UEMBProgressionSubsystem* Progression = World->GetSubsystem<UEMBProgressionSubsystem>())
		{
			// XP et rayon lus dans les réglages de progression (source unique, pas de copie dans EMBDragonDefaults).
			const UEMBProgressionSettings* Settings = UEMBProgressionSettings::Get();
			Progression->NotifyWorldEvent(EMBTags::Event_Boss_Killed_Dragon.GetTag(), this,
				Settings->DragonXP, Settings->ParticipantRadius);
		}
	}

	OnEncounterEnded.Broadcast();
}

// ----------------------------------------------------------------------------------------------
// Recharges et historique
// ----------------------------------------------------------------------------------------------

bool AEMBDragonBoss::IsAttackOnCooldown(const FGameplayTag& AttackTag) const
{
	return GetCooldownRemaining(AttackTag) > 0.f;
}

float AEMBDragonBoss::GetCooldownRemaining(const FGameplayTag& AttackTag) const
{
	const double* EndTime = CooldownEndTimes.Find(AttackTag);
	if (!EndTime || !GetWorld())
	{
		return 0.f;
	}
	// Heure monde (et non un compteur décrémenté au Tick) : une comparaison suffit, aucun coût par frame.
	return static_cast<float>(FMath::Max(0.0, *EndTime - GetWorld()->GetTimeSeconds()));
}

void AEMBDragonBoss::RegisterAttackSelected(const FEMBDragonAttackRow& Row)
{
	if (!HasAuthority() || !GetWorld())
	{
		return;
	}

	PreviousAttackTag = LastAttackTag;
	LastAttackTag = Row.AttackTag;

	// En phase 3 (enragé), les recharges sont réduites de 20 %.
	const double CooldownEnd = GetWorld()->GetTimeSeconds() + static_cast<double>(Row.Cooldown * GetCooldownMultiplier());
	CooldownEndTimes.Add(Row.AttackTag, CooldownEnd);
}

// ----------------------------------------------------------------------------------------------
// Exécution des attaques non latentes
// ----------------------------------------------------------------------------------------------

float AEMBDragonBoss::PlayAttackMontage(UAnimMontage* Montage, const float PlayRate, const FName StartSection)
{
	if (!HasAuthority() || !Montage)
	{
		return 0.f;
	}

	const float Rate = PlayRate > 0.f ? PlayRate : GetAttackPlayRate();
	Multicast_PlayMontage(Montage, Rate, StartSection);

	float Length = Montage->GetPlayLength();
	if (!StartSection.IsNone())
	{
		const int32 SectionIndex = Montage->GetSectionIndex(StartSection);
		if (SectionIndex != INDEX_NONE)
		{
			Length = Montage->GetSectionLength(SectionIndex);
		}
	}
	return Length / Rate;
}

void AEMBDragonBoss::Multicast_PlayMontage_Implementation(UAnimMontage* Montage, const float PlayRate, const FName StartSection)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (!AnimInstance || !Montage)
	{
		return;
	}

	AnimInstance->Montage_Play(Montage, PlayRate);
	if (!StartSection.IsNone())
	{
		AnimInstance->Montage_JumpToSection(StartSection, Montage);
	}
}

void AEMBDragonBoss::Multicast_StopMontages_Implementation(const float BlendOutTime)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr)
	{
		AnimInstance->StopAllMontages(BlendOutTime);
	}
}

float AEMBDragonBoss::BeginTimedAttack(const FEMBDragonAttackRow& Row)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || bIsDead || !World)
	{
		return 0.f;
	}

	CancelTimedAttack(false);

	const float Rate = GetAttackPlayRate();

	// Les instants d'impact et le télégraphe sont mis à l'échelle par le multiplicateur de télégraphe (x0,9 en
	// phase 3), exactement et indépendamment de la vitesse du montage (x1,15). Le montage enragé reste un peu
	// plus rapide visuellement ; l'écart (environ 3 %) est à compenser dans les notifies du montage si besoin.
	const float TimeScale = GetTelegraphMultiplier();
	FTimerManager& TimerManager = World->GetTimerManager();

	// Ajoute un timer d'impact ; les poignées sont conservées pour pouvoir tout annuler d'un coup.
	auto AddTimer = [this, &TimerManager](const FTimerDelegate& Delegate, const float Delay)
	{
		FTimerHandle& Handle = AttackTimers.AddDefaulted_GetRef();
		TimerManager.SetTimer(Handle, Delegate, FMath::Max(Delay, 0.02f), false);
	};

	// Les télégraphes des attaques au montage sont dans le montage : le "play rate" enragé les accélère déjà.
	float MontageDuration = 0.f;
	if (UAnimMontage* Montage = ResolveMontage(Row.Montage))
	{
		MontageDuration = PlayAttackMontage(Montage, Rate);
	}
	Multicast_Telegraph(Row.AttackTag, Row.TelegraphTime * TimeScale);

	float LastEventTime = 0.f;

	if (Row.Kind == EEMBDragonAttackKind::Area && Row.ImpactCount > 0)
	{
		// Pluie de braises : ImpactCount impacts répartis sur ActiveDuration après le télégraphe.
		// Chaque cercle est affiché EmberMarkerLead secondes avant son impact.
		const float Interval = Row.ActiveDuration / static_cast<float>(Row.ImpactCount);
		for (int32 Index = 0; Index < Row.ImpactCount; ++Index)
		{
			const float ImpactTime = Row.TelegraphTime + static_cast<float>(Index) * Interval;
			const float MarkerTime = FMath::Max(0.05f, ImpactTime - EMBDragonDefaults::EmberMarkerLead);
			const float DelayToImpact = (ImpactTime - MarkerTime) * TimeScale;

			AddTimer(FTimerDelegate::CreateUObject(this, &AEMBDragonBoss::HandleEmberMarker, Row.AttackTag, DelayToImpact), MarkerTime * TimeScale);
			LastEventTime = FMath::Max(LastEventTime, ImpactTime);
		}
		SetFacingLocked(false);
	}
	else
	{
		for (const float HitTime : Row.HitTimes)
		{
			AddTimer(FTimerDelegate::CreateUObject(this, &AEMBDragonBoss::ExecuteMeleeHit, Row.AttackTag), HitTime * TimeScale);
			LastEventTime = FMath::Max(LastEventTime, HitTime);
		}

		if (Row.bRearOnly)
		{
			// Fouet caudal : le cône vise l'arrière du dragon. Il ne doit PAS pivoter vers la cible pendant le
			// télégraphe (sinon la queue suivrait le joueur) : orientation figée dès le début de l'attaque.
			SetFacingLocked(true);
		}
		else
		{
			// Le dragon suit sa cible jusqu'à un court instant avant le premier impact, puis l'orientation est figée :
			// c'est ce verrouillage qui rend l'attaque esquivable (lecture de coups).
			SetFacingLocked(false);
			if (Row.HitTimes.Num() > 0)
			{
				const float LockTime = FMath::Max(0.05f, Row.HitTimes[0] - FacingLockLeadTime);
				AddTimer(FTimerDelegate::CreateUObject(this, &AEMBDragonBoss::SetFacingLocked, true), LockTime * TimeScale);
			}
		}
	}

	// Fin d'attaque : déverrouille l'orientation. Sans montage, une récupération de 0,7 s est ajoutée.
	const float RecoveryTail = MontageDuration > 0.f ? 0.3f : 0.7f;
	const float TotalDuration = FMath::Max(MontageDuration, (LastEventTime + RecoveryTail) * TimeScale);
	AddTimer(FTimerDelegate::CreateUObject(this, &AEMBDragonBoss::SetFacingLocked, false), TotalDuration);

	return TotalDuration;
}

void AEMBDragonBoss::CancelTimedAttack(const bool bStopMontage)
{
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		for (FTimerHandle& Handle : AttackTimers)
		{
			TimerManager.ClearTimer(Handle);
		}
	}
	AttackTimers.Reset();

	if (HasAuthority())
	{
		SetFacingLocked(false);
		if (bStopMontage)
		{
			Multicast_StopMontages(0.15f);
		}
	}
}

void AEMBDragonBoss::ExecuteMeleeHit(FGameplayTag AttackTag)
{
	if (bIsDead || bIsStunned || bIsTransitioning)
	{
		return;
	}
	if (const FEMBDragonAttackRow* Row = FindAttackRow(AttackTag))
	{
		ApplyRowShape(*Row);
	}
}

void AEMBDragonBoss::ApplyRowShape(const FEMBDragonAttackRow& Row)
{
	const FEMBHitData Template = MakeHitData(Row);
	const FVector Origin = GetActorLocation();
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();

	TArray<AActor*> Landed;
	if (Row.bRearOnly)
	{
		// Fouet caudal : cône orienté vers l'arrière. |angle| >= RearMinAngle équivaut à un demi-angle de 180 - RearMin.
		Landed = ApplyConeDamage(Origin, -Forward, 180.f - Row.RearMinAngleDeg, Row.MaxRange, Template, false, true);
	}
	else if (Row.HalfAngleDeg >= 179.f)
	{
		Landed = ApplyAreaDamage(Origin, Row.MaxRange, Template);
	}
	else
	{
		Landed = ApplyConeDamage(Origin, Forward, Row.HalfAngleDeg, Row.MaxRange, Template, false, true);
	}

	for (AActor* Victim : Landed)
	{
		if (Row.bAppliesBurn)
		{
			ApplyBurn(Victim);
		}
		if (Row.KnockbackDistance > 0.f)
		{
			ApplyKnockback(Victim, Row.KnockbackDistance);
		}
	}
}

void AEMBDragonBoss::ApplyKnockback(AActor* Target, const float Distance)
{
	ACharacter* Victim = Cast<ACharacter>(Target);
	if (!Victim)
	{
		return;
	}

	// Vitesse approchée pour parcourir "Distance" en KnockbackTravelTime (le frottement n'est pas modélisé) :
	// à valider en jeu. Le serveur lance un personnage possédé par un client : une correction réseau est possible.
	const FVector Direction = (Victim->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	Victim->LaunchCharacter(Direction * (Distance / KnockbackTravelTime) + FVector(0.f, 0.f, 250.f), true, true);
}

void AEMBDragonBoss::HandleEmberMarker(FGameplayTag AttackTag, const float DelayToImpact)
{
	UWorld* World = GetWorld();
	if (!World || bIsDead || bIsStunned || bIsTransitioning)
	{
		return;
	}

	TArray<APawn*> Players;
	CollectAlivePlayerPawns(World, Players);
	if (Players.Num() == 0)
	{
		return;
	}

	// Cible un joueur au hasard, avec une légère anticipation de sa course (plafonnée à 6 m).
	APawn* Victim = Players[RandomStream.RandRange(0, Players.Num() - 1)];
	const FVector Predicted = Victim->GetActorLocation() + (Victim->GetVelocity() * 0.6f).GetClampedToMaxSize(600.f);
	const FVector Ground = ProjectToGround(Predicted);

	const FEMBDragonAttackRow* Row = FindAttackRow(AttackTag);
	const float Radius = (Row && Row->EffectRadius > 0.f) ? Row->EffectRadius : EMBDragonDefaults::EmberImpactRadius;
	Multicast_ShowGroundMarker(Ground, Radius, DelayToImpact, false);

	FTimerHandle& Handle = AttackTimers.AddDefaulted_GetRef();
	World->GetTimerManager().SetTimer(Handle,
		FTimerDelegate::CreateUObject(this, &AEMBDragonBoss::ExecuteEmberImpact, AttackTag, Ground),
		FMath::Max(DelayToImpact, 0.02f), false);
}

void AEMBDragonBoss::ExecuteEmberImpact(FGameplayTag AttackTag, FVector Location)
{
	const FEMBDragonAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row || bIsDead)
	{
		return;
	}

	const float Radius = Row->EffectRadius > 0.f ? Row->EffectRadius : EMBDragonDefaults::EmberImpactRadius;
	ApplyAreaDamage(Location, Radius, MakeHitData(*Row));

	if (UNiagaraSystem* System = ResolveVFX(Row->VFX))
	{
		Multicast_SpawnVFXAtLocation(System, Location, FRotator::ZeroRotator, FVector::OneVector);
	}
}

// ----------------------------------------------------------------------------------------------
// Mouvements scriptés
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::SetFlying(const bool bNewFlying)
{
	if (!HasAuthority() || bIsFlying == bNewFlying)
	{
		return;
	}

	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (!Move)
	{
		return;
	}

	bIsFlying = bNewFlying;
	if (bNewFlying)
	{
		Move->StopMovementImmediately();
		Move->SetMovementMode(MOVE_Flying);
	}
	else
	{
		// MOVE_Falling : le composant repasse tout seul en marche à l'atterrissage.
		Move->SetMovementMode(MOVE_Falling);
	}

	PushStateToBlackboard();
}

bool AEMBDragonBoss::StepFlightToward(const FVector& Destination, const float Speed, const float DeltaSeconds, bool& bOutBlocked)
{
	bOutBlocked = false;

	// Déplacement à vitesse constante par balayage : trajectoire déterministe, durée connue d'avance
	// (nécessaire pour caler l'impact du piqué sur la fin du marqueur au sol). Les clients reçoivent la
	// position via la réplication de mouvement standard ; le lissage à 4 500 cm/s est à valider en jeu.
	const FVector Current = GetActorLocation();
	const FVector Next = FMath::VInterpConstantTo(Current, Destination, DeltaSeconds, Speed);

	FHitResult SweepHit;
	SetActorLocation(Next, true, &SweepHit, ETeleportType::None);
	bOutBlocked = SweepHit.bBlockingHit;

	// Vitesse renseignée à partir du déplacement réel : elle est répliquée avec le mouvement, ce qui permet aux
	// clients d'extrapoler la trajectoire (indispensable à 4 500 cm/s). Le freinage de vol du composant est réglé
	// très haut dans le constructeur pour qu'il n'ajoute pas un second déplacement à celui-ci.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		if (DeltaSeconds > KINDA_SMALL_NUMBER)
		{
			Move->Velocity = (GetActorLocation() - Current) / static_cast<double>(DeltaSeconds);
		}
	}

	return GetActorLocation().Equals(Destination, 25.f);
}

void AEMBDragonBoss::SetFacingLocked(const bool bLocked)
{
	bFacingLocked = bLocked;
	if (AEMBDragonAIController* DragonController = Cast<AEMBDragonAIController>(GetController()))
	{
		DragonController->SetFacingLocked(bLocked);
	}
}

void AEMBDragonBoss::RotateYawToward(const float TargetYawDeg, const float DeltaSeconds, const float DegreesPerSecond)
{
	const FRotator Current = GetActorRotation();
	FRotator Next = FMath::RInterpConstantTo(Current, FRotator(0.f, TargetYawDeg, 0.f), DeltaSeconds, DegreesPerSecond);
	Next.Pitch = 0.f;
	Next.Roll = 0.f;
	SetActorRotation(Next);
}

FVector AEMBDragonBoss::GetBreathOrigin() const
{
	const USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp && MeshComp->DoesSocketExist(MouthSocketName))
	{
		return MeshComp->GetSocketLocation(MouthSocketName);
	}
	return GetActorLocation() + GetActorForwardVector() * 600.f + FVector(0.f, 0.f, 300.f);
}

FVector AEMBDragonBoss::ProjectToGround(const FVector& Location) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return Location;
	}

	FHitResult GroundHit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBDragonGroundProbe), false, this);
	const FVector Start = Location + FVector(0.f, 0.f, 1000.f);
	const FVector End = Location - FVector(0.f, 0.f, 6000.f);
	return World->LineTraceSingleByChannel(GroundHit, Start, End, ECC_Visibility, Params) ? GroundHit.ImpactPoint : Location;
}

// ----------------------------------------------------------------------------------------------
// Contexte de combat
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::GetDistanceAndAngleTo(const AActor* Target, float& OutDistance, float& OutSignedAngleDeg) const
{
	OutDistance = 0.f;
	OutSignedAngleDeg = 0.f;
	if (!Target)
	{
		return;
	}

	const FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
	OutDistance = static_cast<float>(ToTarget.Size2D());

	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();
	const FVector Direction = ToTarget.GetSafeNormal2D();
	// Produit vectoriel Z > 0 : la cible est à droite du dragon (repère UE main gauche).
	OutSignedAngleDeg = static_cast<float>(FMath::RadiansToDegrees(FMath::Atan2(
		FVector::CrossProduct(Forward, Direction).Z, FVector::DotProduct(Forward, Direction))));
}

void AEMBDragonBoss::CollectAlivePlayerPawns(const UWorld* World, TArray<APawn*>& OutPawns)
{
	if (!World)
	{
		return;
	}

	// Itération des PlayerControllers (4 au maximum) : bien moins cher qu'un GetAllActorsOfClass.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* PlayerController = It->Get();
		APawn* Pawn = PlayerController ? PlayerController->GetPawn() : nullptr;
		if (!Pawn)
		{
			continue;
		}
		const UEMBStatsComponent* Stats = Pawn->FindComponentByClass<UEMBStatsComponent>();
		if (Stats && Stats->IsDead())
		{
			continue;
		}
		OutPawns.Add(Pawn);
	}
}

int32 AEMBDragonBoss::CountPlayersWithin(const float Radius) const
{
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(GetWorld(), Players);

	const float RadiusSquared = FMath::Square(Radius);
	int32 Count = 0;
	for (const APawn* Pawn : Players)
	{
		if (FVector::DistSquared2D(Pawn->GetActorLocation(), GetActorLocation()) <= RadiusSquared)
		{
			++Count;
		}
	}
	return Count;
}

void AEMBDragonBoss::AddThreat(AActor* Source, const float Amount)
{
	if (!Source || Amount <= 0.f || !GetWorld())
	{
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	FEMBDragonThreatEntry& Entry = ThreatTable.FindOrAdd(Source);

	// Décroissance exponentielle appliquée à l'écriture : la lecture reste O(1) et sans Tick.
	const float Decay = FMath::Pow(0.5f, static_cast<float>(Now - Entry.LastUpdateTime) / ThreatHalfLife);
	Entry.Value = Entry.Value * Decay + Amount;
	Entry.LastUpdateTime = Now;
}

float AEMBDragonBoss::GetThreatScore(AActor* Player) const
{
	if (!Player || !GetWorld())
	{
		return 0.f;
	}

	const FEMBDragonThreatEntry* Entry = ThreatTable.Find(Player);
	if (!Entry)
	{
		return 0.f;
	}

	const float Elapsed = static_cast<float>(GetWorld()->GetTimeSeconds() - Entry->LastUpdateTime);
	return Entry->Value * FMath::Pow(0.5f, Elapsed / ThreatHalfLife) / ThreatDamageForOneScorePoint;
}

// ----------------------------------------------------------------------------------------------
// Dégâts de zone
// ----------------------------------------------------------------------------------------------

FEMBHitData AEMBDragonBoss::MakeHitData(const FEMBDragonAttackRow& Row)
{
	FEMBHitData HitData;
	HitData.Instigator = this;
	HitData.Damage = Row.Damage;
	HitData.PoiseDamage = Row.PoiseDamage;
	HitData.StaminaDamage = Row.StaminaDamage; // 0 : Damage x 0,5 sur garde ; souffles : 10 par tick (v1.1)
	HitData.DamageType = Row.DamageType;
	HitData.bUnblockable = Row.bUnblockable;
	HitData.bUnparryable = !Row.bParryable;
	HitData.AttackTag = Row.AttackTag;
	return HitData;
}

void AEMBDragonBoss::GatherPlayerPawnsInSphere(const FVector& Center, const float Radius, TArray<APawn*>& OutPawns) const
{
	const UWorld* World = GetWorld();
	if (!World || Radius <= 0.f)
	{
		return;
	}

	// Optimisation : au plus 4 joueurs, déjà listés par CollectAlivePlayerPawns. Un test distance - rayon de
	// capsule est bien moins cher (et plus fiable : pas de dépendance aux presets de collision) qu'une requête
	// d'overlap physique, y compris pour un rayon de 150 m (Cataclysm). Seuls les Pawns joueurs sont des cibles.
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(World, Players);

	for (APawn* Pawn : Players)
	{
		if (Pawn == this)
		{
			continue;
		}
		const double Distance = FVector::Dist(Pawn->GetActorLocation(), Center);
		if (Distance - static_cast<double>(Pawn->GetSimpleCollisionRadius()) <= static_cast<double>(Radius))
		{
			OutPawns.Add(Pawn);
		}
	}
}

bool AEMBDragonBoss::HasLineOfSight(const FVector& Origin, const AActor* Target) const
{
	const UWorld* World = GetWorld();
	if (!World || !Target)
	{
		return false;
	}

	// Canal de trace configurable (LineOfSightChannel, défaut Visibility) : les piliers et les murs de l'arène
	// doivent le bloquer. Un canal de trace projet dédié "Cover" est plus robuste que Visibility.
	FHitResult BlockingHit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBDragonLineOfSight), false, this);
	Params.AddIgnoredActor(Target);
	return !World->LineTraceSingleByChannel(BlockingHit, Origin, Target->GetActorLocation(), LineOfSightChannel, Params);
}

TArray<AActor*> AEMBDragonBoss::ApplyHitToPawns(const TArray<APawn*>& Pawns, const FVector& Origin, const FEMBHitData& HitTemplate, const bool bRequireLineOfSight)
{
	TArray<AActor*> Landed;
	for (APawn* Pawn : Pawns)
	{
		if (bRequireLineOfSight && !HasLineOfSight(Origin, Pawn))
		{
			continue;
		}

		FEMBHitData PerTarget = HitTemplate;
		PerTarget.Instigator = this;
		PerTarget.HitLocation = Pawn->GetActorLocation();
		PerTarget.HitDirection = (Pawn->GetActorLocation() - Origin).GetSafeNormal();

		const EEMBHitResult Result = UEMBCombatComponent::ApplyHitToActor(Pawn, PerTarget);
		if (Result == EEMBHitResult::Hit || Result == EEMBHitResult::Killed || Result == EEMBHitResult::GuardBroken)
		{
			Landed.Add(Pawn);
		}
	}
	return Landed;
}

TArray<AActor*> AEMBDragonBoss::ApplyAreaDamage(const FVector& Center, const float Radius, const FEMBHitData& HitTemplate, const bool bRequireLineOfSight)
{
	// Autorité serveur uniquement (les clients ne résolvent jamais de dégâts).
	if (GetNetMode() == NM_Client || !HasAuthority())
	{
		return TArray<AActor*>();
	}

	TArray<APawn*> Candidates;
	GatherPlayerPawnsInSphere(Center, Radius, Candidates);
	return ApplyHitToPawns(Candidates, Center, HitTemplate, bRequireLineOfSight);
}

TArray<AActor*> AEMBDragonBoss::ApplyConeDamage(const FVector& Origin, const FVector& Direction, const float HalfAngleDeg, const float Length, const FEMBHitData& HitTemplate, const bool bRequireLineOfSight, const bool bHorizontalOnly)
{
	if (GetNetMode() == NM_Client || !HasAuthority() || Length <= 0.f)
	{
		return TArray<AActor*>();
	}

	// bHorizontalOnly : cône plat au sol (mêlée) ; sinon cône 3D (souffles inclinés vers le sol).
	const FVector ConeDirection = bHorizontalOnly ? Direction.GetSafeNormal2D() : Direction.GetSafeNormal();
	if (ConeDirection.IsNearlyZero())
	{
		return TArray<AActor*>();
	}

	// Pré-filtre par distance (sphère), puis test angulaire exact sur les 4 joueurs au maximum.
	TArray<APawn*> InSphere;
	GatherPlayerPawnsInSphere(Origin, Length, InSphere);

	TArray<APawn*> InCone;
	for (APawn* Pawn : InSphere)
	{
		FVector ToTarget = Pawn->GetActorLocation() - Origin;
		if (bHorizontalOnly)
		{
			ToTarget.Z = 0.0;
		}
		const float Distance = static_cast<float>(ToTarget.Size());

		float AngleDeg = 0.f;
		if (Distance > KINDA_SMALL_NUMBER)
		{
			const float CosAngle = FMath::Clamp(static_cast<float>(FVector::DotProduct(ConeDirection, ToTarget / Distance)), -1.f, 1.f);
			AngleDeg = FMath::RadiansToDegrees(FMath::Acos(CosAngle));
		}

		// Marge angulaire = angle sous-tendu par la capsule du joueur : un joueur dont le bord touche le cône est touché.
		const float SlackDeg = FMath::RadiansToDegrees(FMath::Atan2(Pawn->GetSimpleCollisionRadius(), FMath::Max(Distance, 1.f)));
		if (AngleDeg <= HalfAngleDeg + SlackDeg)
		{
			InCone.Add(Pawn);
		}
	}

	return ApplyHitToPawns(InCone, Origin, HitTemplate, bRequireLineOfSight);
}

// ----------------------------------------------------------------------------------------------
// Brûlure et zones de feu
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::ApplyBurn(AActor* Target)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !Target || !World)
	{
		return;
	}

	// Simple : une table "cible -> heure de fin" et UN seul timer répétitif pour toutes les cibles (au lieu d'un
	// timer par joueur). Réappliquer la brûlure rafraîchit sa durée sans la cumuler.
	BurnEndTimes.Add(Target, World->GetTimeSeconds() + EMBDragonDefaults::BurnDuration);

	FTimerManager& TimerManager = World->GetTimerManager();
	if (!TimerManager.IsTimerActive(BurnTimerHandle))
	{
		TimerManager.SetTimer(BurnTimerHandle, this, &AEMBDragonBoss::TickBurn, EMBDragonDefaults::BurnTickInterval, true);
	}
}

void AEMBDragonBoss::TickBurn()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();

	// Brûlure : dégâts continus, ni parables ni blocables, sans dégâts de posture. Un joueur en i-frames de
	// roulade esquive le tick correspondant (acceptable : pas de mécanique d'extinction dans le GDD).
	FEMBHitData BurnHit;
	BurnHit.Instigator = this;
	BurnHit.Damage = EMBDragonDefaults::BurnDps * EMBDragonDefaults::BurnTickInterval;
	BurnHit.PoiseDamage = 0.f;
	BurnHit.DamageType = EEMBDamageType::Fire;
	BurnHit.bUnblockable = true;
	BurnHit.bUnparryable = true;
	BurnHit.bDamageOverTime = true; // v1.1 : dégâts directs, ni esquive, ni counter-hit, ni multicast

	for (auto It = BurnEndTimes.CreateIterator(); It; ++It)
	{
		AActor* Victim = It.Key().Get();
		if (!Victim || Now >= It.Value())
		{
			It.RemoveCurrent();
			continue;
		}

		BurnHit.HitLocation = Victim->GetActorLocation();
		BurnHit.HitDirection = FVector::UpVector;
		const EEMBHitResult Result = UEMBCombatComponent::ApplyHitToActor(Victim, BurnHit);
		if (Result == EEMBHitResult::Killed || Result == EEMBHitResult::Ignored)
		{
			It.RemoveCurrent();
		}
	}

	if (BurnEndTimes.Num() == 0)
	{
		World->GetTimerManager().ClearTimer(BurnTimerHandle);
	}
}

void AEMBDragonBoss::AddFireZone(const FVector& Center, const float Radius, const float Duration, const float DamagePerSecond)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World || Radius <= 0.f)
	{
		return;
	}

	FEMBDragonFireZone& Zone = FireZones.AddDefaulted_GetRef();
	Zone.Center = Center;
	Zone.Radius = Radius;
	Zone.EndTime = World->GetTimeSeconds() + Duration;
	Zone.DamagePerSecond = DamagePerSecond;

	FTimerManager& TimerManager = World->GetTimerManager();
	if (!TimerManager.IsTimerActive(FireZoneTimerHandle))
	{
		TimerManager.SetTimer(FireZoneTimerHandle, this, &AEMBDragonBoss::TickFireZones, FireZoneTickInterval, true);
	}
}

void AEMBDragonBoss::TickFireZones()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	FireZones.RemoveAll([Now](const FEMBDragonFireZone& Zone) { return Now >= Zone.EndTime; });

	if (FireZones.Num() == 0)
	{
		World->GetTimerManager().ClearTimer(FireZoneTimerHandle);
		return;
	}

	// Optimisation : pas de requête physique par zone. On teste les 4 joueurs au maximum contre chaque zone
	// (distance à plat) : une dizaine de zones x 4 joueurs x 4 ticks/s = négligeable.
	// Un joueur ne subit au plus qu'un tick par intervalle, même s'il se tient dans plusieurs zones superposées.
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(World, Players);

	for (APawn* Pawn : Players)
	{
		for (const FEMBDragonFireZone& Zone : FireZones)
		{
			const float ReachRadius = Zone.Radius + Pawn->GetSimpleCollisionRadius();
			const bool bInside = FVector::DistSquared2D(Pawn->GetActorLocation(), Zone.Center) <= FMath::Square(ReachRadius)
				&& FMath::Abs(Pawn->GetActorLocation().Z - Zone.Center.Z) <= 300.f;
			if (!bInside)
			{
				continue;
			}

			FEMBHitData ZoneHit;
			ZoneHit.Instigator = this;
			ZoneHit.Damage = Zone.DamagePerSecond * FireZoneTickInterval;
			ZoneHit.PoiseDamage = 0.f;
			ZoneHit.DamageType = EEMBDamageType::Fire;
			ZoneHit.bUnblockable = true;
			ZoneHit.bUnparryable = true;
			ZoneHit.bDamageOverTime = true; // v1.1
			ZoneHit.HitLocation = Pawn->GetActorLocation();
			ZoneHit.HitDirection = FVector::UpVector;
			UEMBCombatComponent::ApplyHitToActor(Pawn, ZoneHit);
			break;
		}
	}
}

// ----------------------------------------------------------------------------------------------
// Effets cosmétiques répliqués
// ----------------------------------------------------------------------------------------------

void AEMBDragonBoss::Multicast_StartBreathVFX_Implementation(UNiagaraSystem* System, const FName SocketName)
{
	// Serveur dédié : aucun rendu, donc aucun VFX à instancier.
	if (IsNetMode(NM_DedicatedServer) || !System || !GetMesh())
	{
		return;
	}

	if (ActiveBreathVFX)
	{
		ActiveBreathVFX->Deactivate();
		ActiveBreathVFX = nullptr;
	}

	// NAME_None : on attache le souffle à la gueule du dragon.
	const FName AttachSocket = SocketName.IsNone() ? MouthSocketName : SocketName;
	ActiveBreathVFX = UNiagaraFunctionLibrary::SpawnSystemAttached(System, GetMesh(), AttachSocket, FVector::ZeroVector,
		FRotator::ZeroRotator, EAttachLocation::SnapToTarget, true);
}

void AEMBDragonBoss::Multicast_StopBreathVFX_Implementation()
{
	if (ActiveBreathVFX)
	{
		ActiveBreathVFX->Deactivate();
		ActiveBreathVFX = nullptr;
	}
}

void AEMBDragonBoss::Multicast_SpawnVFXAtLocation_Implementation(UNiagaraSystem* System, const FVector Location, const FRotator Rotation, const FVector Scale)
{
	if (IsNetMode(NM_DedicatedServer) || !System)
	{
		return;
	}
	UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, Rotation, Scale);
}

void AEMBDragonBoss::Multicast_SpawnFireZoneVFX_Implementation(UNiagaraSystem* System, const TArray<FVector>& Locations)
{
	// Les zones de feu (dégâts) n'existent que sur le serveur : ce multicast, envoyé une seule fois par traînée,
	// fait apparaître leur VFX localement chez tous les clients. Perte tolérée : seul le visuel manque.
	if (IsNetMode(NM_DedicatedServer) || !System)
	{
		return;
	}
	for (const FVector& Location : Locations)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(this, System, Location, FRotator::ZeroRotator, FVector::OneVector);
	}
}

void AEMBDragonBoss::Multicast_Telegraph_Implementation(const FGameplayTag AttackTag, const float Duration)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnAttackTelegraph(AttackTag, Duration);
	}
}

void AEMBDragonBoss::Multicast_ShowGroundMarker_Implementation(const FVector Location, const float Radius, const float Duration, const bool bFlash)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnShowGroundMarker(Location, Radius, Duration, bFlash);
	}
}
