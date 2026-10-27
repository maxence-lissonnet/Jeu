// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCreatureBase.h"

#include "Emberfall.h"
#include "EMBBossBase.h"
#include "EMBCombatComponent.h"
#include "EMBCreatureAIController.h"
#include "EMBCreatureSignificance.h"
#include "EMBCreatureSpawner.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBResourceNodeSubsystem.h"
#include "EMBStatsComponent.h"

#include "AIController.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "BrainComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Distance à plat (cm) et angle absolu (degrés, 0 = face à la cible) de Self vers Target. */
	void GetPlanarDistanceAndAngle(const AActor& Self, const AActor& Target, float& OutDistance, float& OutAngleDeg)
	{
		const FVector ToTarget = Target.GetActorLocation() - Self.GetActorLocation();
		OutDistance = static_cast<float>(ToTarget.Size2D());

		const FVector Forward = Self.GetActorForwardVector().GetSafeNormal2D();
		const FVector Direction = ToTarget.GetSafeNormal2D();
		const float Cosine = FMath::Clamp(static_cast<float>(FVector::DotProduct(Forward, Direction)), -1.f, 1.f);
		OutAngleDeg = FMath::RadiansToDegrees(FMath::Acos(Cosine));
	}

	/** Le joueur est-il dans la forme de l'attaque ? Bond : sphère (EffectRadius). Sinon cône à plat de longueur MaxRange. */
	bool IsPawnInAttackShape(const FEMBCreatureAttackRow& Row, const FVector& Origin, const FVector& Forward2D, const APawn& Pawn)
	{
		const float CapsuleRadius = Pawn.GetSimpleCollisionRadius();
		FVector ToPawn = Pawn.GetActorLocation() - Origin;

		if (Row.Kind == EEMBDragonAttackKind::Charge)
		{
			const float Radius = Row.EffectRadius > 0.f ? Row.EffectRadius : Row.MaxRange;
			return ToPawn.Size() - CapsuleRadius <= Radius;
		}

		ToPawn.Z = 0.0;
		const float Distance = static_cast<float>(ToPawn.Size());
		if (Distance - CapsuleRadius > Row.MaxRange)
		{
			return false;
		}
		if (Row.HalfAngleDeg >= 179.f || Distance <= KINDA_SMALL_NUMBER)
		{
			return true;
		}

		const float Cosine = FMath::Clamp(static_cast<float>(FVector::DotProduct(Forward2D, ToPawn / Distance)), -1.f, 1.f);
		const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(Cosine));
		// Marge angulaire = angle sous-tendu par la capsule du joueur.
		const float SlackDeg = FMath::RadiansToDegrees(FMath::Atan2(CapsuleRadius, FMath::Max(Distance, 1.f)));
		return AngleDeg <= Row.HalfAngleDeg + SlackDeg;
	}
}

AEMBCreatureBase::AEMBCreatureBase()
{
	// Aucun Tick : attaques, étourdissement et mort fonctionnent par timers et par les tâches du Behavior Tree.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetNetUpdateFrequency(20.f);

	AIControllerClass = AEMBCreatureAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	StatsComponent = CreateDefaultSubobject<UEMBStatsComponent>(TEXT("StatsComponent"));
	StatsComponent->SetIsReplicated(true);

	CreatureTable = TSoftObjectPtr<UDataTable>(EMBCreatureData::GetDefaultTablePath());

	bUseControllerRotationYaw = false;
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->bOrientRotationToMovement = false;
		Move->bUseControllerDesiredRotation = true;
		Move->RotationRate = FRotator(0.f, 360.f, 0.f);
		Move->BrakingDecelerationFlying = 2000.f;
	}

	// Palier "plein" par défaut ; UEMBCreatureSignificance rétrograde le mesh des créatures lointaines.
	// Sur serveur dédié, les os doivent être à jour tant qu'un joueur est à portée de coup (traces de mêlée).
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

		// La lame des joueurs balaie EMBMeleeHit (ECC_GameTraceChannel1, Ignore par défaut) : le mesh (Physics Asset) y
		// répond en Overlap, la capsule l'ignore (voir EMBCombatComponent.h).
		MeshComp->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	}
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Ignore);
	}
}

void AEMBCreatureBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Réplication minimale : PV et poise via UEMBStatsComponent, mouvement via ACharacter. Reste l'état de mort.
	DOREPLIFETIME(AEMBCreatureBase, bIsDead);
}

// ----------------------------------------------------------------------------------------------
// Cycle de vie
// ----------------------------------------------------------------------------------------------

bool AEMBCreatureBase::LoadRow(FEMBCreatureRow& OutRow) const
{
	return EMBCreatureData::FindRow(CreatureTable, CreatureRowName, OutRow);
}

bool AEMBCreatureBase::FindRowForClass(const UClass* CreatureClass, FEMBCreatureRow& OutRow)
{
	if (!CreatureClass || !CreatureClass->IsChildOf(StaticClass()))
	{
		return false;
	}
	const AEMBCreatureBase* Default = Cast<AEMBCreatureBase>(CreatureClass->GetDefaultObject());
	return Default && Default->LoadRow(OutRow);
}

void AEMBCreatureBase::PreInitializeComponents()
{
	Super::PreInitializeComponents();

	// Avant InitializeComponent du composant de stats : il copie MaxHealth / MaxPoise dans les jauges courantes.
	// Serveur et clients lisent la même ligne (propriété de classe) : les maxima concordent avant la réplication.
	bHasRow = LoadRow(CachedRow);
	if (!bHasRow)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBCreatureBase %s : ligne '%s' introuvable, valeurs par defaut."), *GetName(), *CreatureRowName.ToString());
		return;
	}

	if (StatsComponent)
	{
		StatsComponent->MaxHealth = CachedRow.MaxHealth;
		StatsComponent->MaxPoise = CachedRow.MaxPoise;
		StatsComponent->PoiseRegenRate = EMBCreatureDefaults::PoiseRegenRate;
		StatsComponent->PoiseRegenDelay = EMBCreatureDefaults::PoiseRegenDelay;
	}

	SetRunning(false);
	HomeLocation = GetActorLocation(); // point d'apparition, connu avant le possess du contrôleur

	// Créature volante : le mode de déplacement par défaut est le vol (appliqué à l'initialisation du composant).
	if (CachedRow.bFlying)
	{
		if (UCharacterMovementComponent* Move = GetCharacterMovement())
		{
			Move->DefaultLandMovementMode = MOVE_Flying;
		}
	}

	// Vol bas : le mesh est surélevé (cosmétique, serveur ET clients), la capsule reste sur la navigation.
	if (CachedRow.bFlying && HoverHeight > 0.f)
	{
		if (USkeletalMeshComponent* MeshComp = GetMesh())
		{
			MeshComp->AddRelativeLocation(FVector(0.f, 0.f, HoverHeight));
		}
	}
}

void AEMBCreatureBase::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	if (StatsComponent)
	{
		// Délégués déclenchés sur le serveur uniquement (ApplyDamage / ApplyPoiseDamage sont autoritaires).
		StatsComponent->OnDeath.AddUniqueDynamic(this, &AEMBCreatureBase::HandleStatsDeath);
		StatsComponent->OnPoiseBroken.AddUniqueDynamic(this, &AEMBCreatureBase::HandlePoiseBroken);
	}
}

void AEMBCreatureBase::BeginPlay()
{
	Super::BeginPlay();

	RandomStream.GenerateNewSeed();

	// Budget serveur et paliers de détail : serveur ET clients (les clients ne gèrent que l'animation).
	if (UWorld* World = GetWorld())
	{
		if (UEMBCreatureSignificance* Significance = World->GetSubsystem<UEMBCreatureSignificance>())
		{
			Significance->RegisterCreature(this);
		}
	}

	if (HasAuthority())
	{
		BuildAttackCache();
		PreloadAttackAssets();
	}
}

void AEMBCreatureBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearAllTimersForObject(this);
		if (UEMBCreatureSignificance* Significance = World->GetSubsystem<UEMBCreatureSignificance>())
		{
			Significance->UnregisterCreature(this);
		}
	}
	PreloadHandle.Reset();

	Super::EndPlay(EndPlayReason);
}

void AEMBCreatureBase::SetOwningSpawner(AEMBCreatureSpawner* Spawner)
{
	OwningSpawner = Spawner;
}

void AEMBCreatureBase::ReturnToSpawner()
{
	if (!HasAuthority() || bIsDead || !OwningSpawner.IsValid())
	{
		return;
	}
	OwningSpawner->NotifyChildReturned(this);
	Destroy();
}

// ----------------------------------------------------------------------------------------------
// Données d'attaque
// ----------------------------------------------------------------------------------------------

void AEMBCreatureBase::BuildAttackCache()
{
	AttackRows.Reset();
	AttackRowByTag.Reset();
	AttackTableAsset = nullptr;

	if (!bHasRow || CachedRow.AttackTable.IsNull())
	{
		return;
	}

	UDataTable* Table = CachedRow.AttackTable.LoadSynchronous();
	if (!Table || Table->GetRowStruct() != FEMBCreatureAttackRow::StaticStruct())
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBCreatureBase %s : table d'attaques %s introuvable ou de mauvais type."), *GetName(), *CachedRow.AttackTable.ToString());
		return;
	}
	AttackTableAsset = Table; // référence forte : les pointeurs de lignes restent valides

	TArray<FEMBCreatureAttackRow*> Rows;
	Table->GetAllRows<FEMBCreatureAttackRow>(TEXT("AEMBCreatureBase::BuildAttackCache"), Rows);
	for (const FEMBCreatureAttackRow* Row : Rows)
	{
		if (Row && Row->AttackTag.IsValid())
		{
			AttackRows.Add(Row);
			AttackRowByTag.Add(Row->AttackTag, Row);
		}
	}
}

void AEMBCreatureBase::PreloadAttackAssets()
{
	// Montages en soft pointers : chargement asynchrone dès le BeginPlay pour éviter un hitch à la première attaque.
	TArray<FSoftObjectPath> Paths;
	for (const FEMBCreatureAttackRow* Row : AttackRows)
	{
		if (!Row->Montage.IsNull())
		{
			Paths.AddUnique(Row->Montage.ToSoftObjectPath());
		}
	}
	if (Paths.Num() > 0)
	{
		PreloadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Paths);
	}
}

UAnimMontage* AEMBCreatureBase::ResolveMontage(const TSoftObjectPtr<UAnimMontage>& Soft) const
{
	if (Soft.IsNull())
	{
		return nullptr;
	}
	UAnimMontage* Montage = Soft.Get();
	if (!Montage)
	{
		Montage = Soft.LoadSynchronous(); // repli : le préchargement n'est pas terminé
	}
	return Montage;
}

const FEMBCreatureAttackRow* AEMBCreatureBase::FindAttackRow(const FGameplayTag& AttackTag) const
{
	const FEMBCreatureAttackRow* const* Found = AttackRowByTag.Find(AttackTag);
	return Found ? *Found : nullptr;
}

bool AEMBCreatureBase::IsAttackOnCooldown(const FGameplayTag& AttackTag) const
{
	const double* EndTime = CooldownEndTimes.Find(AttackTag);
	return EndTime && GetWorld() && GetWorld()->GetTimeSeconds() < *EndTime;
}

// ----------------------------------------------------------------------------------------------
// Sélection des attaques
// ----------------------------------------------------------------------------------------------

float AEMBCreatureBase::ComputeAttackWeight(const FEMBCreatureAttackRow& Row, const float Distance, const float AngleDeg) const
{
	// Les créatures n'ont qu'une phase : la ligne doit autoriser la phase 1.
	if (Row.bScripted || !Row.IsAllowedInPhase(1) || IsAttackOnCooldown(Row.AttackTag))
	{
		return 0.f;
	}
	if (Distance < Row.MinRange || (Row.MaxRange > 0.f && Distance > Row.MaxRange))
	{
		return 0.f;
	}
	if (Row.HalfAngleDeg < 179.f && AngleDeg > Row.HalfAngleDeg)
	{
		return 0.f;
	}

	float Weight = Row.BaseWeight;
	if (Row.AttackTag == LastAttackTag)
	{
		Weight *= EMBDragonDefaults::RepeatPenaltyLast; // anti-répétition (comme les boss)
	}
	return Weight;
}

bool AEMBCreatureBase::HasAttackInRange(const AActor* Target) const
{
	if (!Target || bIsDead)
	{
		return false;
	}

	float Distance = 0.f;
	float AngleDeg = 0.f;
	GetPlanarDistanceAndAngle(*this, *Target, Distance, AngleDeg);

	for (const FEMBCreatureAttackRow* Row : AttackRows)
	{
		if (ComputeAttackWeight(*Row, Distance, AngleDeg) > 0.f)
		{
			return true;
		}
	}
	return false;
}

const FEMBCreatureAttackRow* AEMBCreatureBase::SelectAttack(const AActor* Target)
{
	float Distance = 0.f;
	float AngleDeg = 0.f;
	GetPlanarDistanceAndAngle(*this, *Target, Distance, AngleDeg);

	TArray<const FEMBCreatureAttackRow*, TInlineAllocator<8>> Candidates;
	TArray<float, TInlineAllocator<8>> Weights;
	float TotalWeight = 0.f;
	for (const FEMBCreatureAttackRow* Row : AttackRows)
	{
		const float Weight = ComputeAttackWeight(*Row, Distance, AngleDeg);
		if (Weight > 0.f)
		{
			Candidates.Add(Row);
			Weights.Add(Weight);
			TotalWeight += Weight;
		}
	}
	if (Candidates.Num() == 0)
	{
		return nullptr;
	}

	// Tirage pondéré : on parcourt le cumul des poids jusqu'à dépasser la valeur tirée.
	const float Roll = RandomStream.FRandRange(0.f, TotalWeight);
	float Cumulative = 0.f;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		Cumulative += Weights[Index];
		if (Roll <= Cumulative)
		{
			return Candidates[Index];
		}
	}
	return Candidates.Last(); // garde contre l'arrondi flottant
}

void AEMBCreatureBase::RegisterAttackSelected(const FEMBCreatureAttackRow& Row)
{
	LastAttackTag = Row.AttackTag;
	if (const UWorld* World = GetWorld())
	{
		CooldownEndTimes.Add(Row.AttackTag, World->GetTimeSeconds() + static_cast<double>(Row.Cooldown));
	}
}

// ----------------------------------------------------------------------------------------------
// Exécution des attaques
// ----------------------------------------------------------------------------------------------

float AEMBCreatureBase::StartAttackOn(AActor* Target)
{
	if (!HasAuthority() || bIsDead || bIsStaggered || bAttacking || !Target)
	{
		return 0.f;
	}

	const FEMBCreatureAttackRow* Row = SelectAttack(Target);
	if (!Row)
	{
		return 0.f;
	}

	RegisterAttackSelected(*Row);
	return ExecuteAttack(*Row, Target);
}

float AEMBCreatureBase::ExecuteAttack(const FEMBCreatureAttackRow& Row, AActor* Target)
{
	CancelAttack(false);
	bAttacking = true;

	// Montage (durée réelle) puis télégraphe visuel chez tous les clients.
	float MontageDuration = 0.f;
	if (UAnimMontage* Montage = ResolveMontage(Row.Montage))
	{
		MontageDuration = Montage->GetPlayLength();
		Multicast_PlayAttackMontage(Montage, 1.f); // Unreliable : cosmétique ponctuel, perte tolérée
	}
	Multicast_Telegraph(Row.AttackTag, Row.TelegraphTime);

	float LastEventTime = Row.TelegraphTime;

	// Bond : lancé à la fin du télégraphe, il atteint la cible à HitTimes[0].
	const bool bLunge = Row.Kind == EEMBDragonAttackKind::Charge;
	if (bLunge)
	{
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBCreatureBase::ExecuteLunge, Row.AttackTag, TWeakObjectPtr<AActor>(Target)), Row.TelegraphTime);
	}

	// Impacts programmés. Sans HitTimes, un impact unique à la fin du télégraphe.
	float FirstHitTime = Row.TelegraphTime;
	if (Row.HitTimes.Num() == 0)
	{
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBCreatureBase::ExecuteHit, Row.AttackTag, 0), Row.TelegraphTime);
	}
	else
	{
		FirstHitTime = Row.HitTimes[0];
		for (int32 HitIndex = 0; HitIndex < Row.HitTimes.Num(); ++HitIndex)
		{
			AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBCreatureBase::ExecuteHit, Row.AttackTag, HitIndex), Row.HitTimes[HitIndex]);
			LastEventTime = FMath::Max(LastEventTime, Row.HitTimes[HitIndex]);
		}
	}

	// La créature suit sa cible du regard, puis l'orientation est figée juste avant le premier impact : c'est ce
	// verrouillage qui rend l'attaque esquivable. Le bond verrouille lui-même (ExecuteLunge).
	if (!bLunge)
	{
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBCreatureBase::LockFacing), FMath::Max(0.05f, FirstHitTime - EMBCreatureDefaults::FacingLockLeadTime));
	}

	const float TotalDuration = FMath::Max(MontageDuration, LastEventTime + EMBCreatureDefaults::AttackRecoveryTail);
	AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBCreatureBase::FinishAttack), TotalDuration);
	return TotalDuration;
}

void AEMBCreatureBase::AddAttackTimer(const FTimerDelegate& Delegate, const float Delay)
{
	if (UWorld* World = GetWorld())
	{
		FTimerHandle& Handle = AttackTimers.AddDefaulted_GetRef();
		World->GetTimerManager().SetTimer(Handle, Delegate, FMath::Max(Delay, 0.02f), false);
	}
}

void AEMBCreatureBase::CancelAttack(const bool bStopMontage)
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
		const bool bWasAttacking = bAttacking;
		bAttacking = false;
		SetFacingLocked(false);
		if (bStopMontage && bWasAttacking)
		{
			Multicast_StopMontages(0.15f);
		}
	}
}

void AEMBCreatureBase::FinishAttack()
{
	// Dernier timer de l'attaque : tous les autres ont déjà expiré.
	AttackTimers.Reset();
	bAttacking = false;
	SetFacingLocked(false);
}

void AEMBCreatureBase::LockFacing()
{
	SetFacingLocked(true);
}

void AEMBCreatureBase::SetFacingLocked(const bool bLocked)
{
	if (AEMBCreatureAIController* AIController = Cast<AEMBCreatureAIController>(GetController()))
	{
		AIController->SetFacingLocked(bLocked);
	}
}

FEMBHitData AEMBCreatureBase::MakeHitData(const FEMBCreatureAttackRow& Row)
{
	FEMBHitData HitData;
	HitData.Instigator = this;
	HitData.Damage = Row.Damage;
	HitData.PoiseDamage = Row.PoiseDamage;
	HitData.StaminaDamage = Row.StaminaDamage; // 0 : Damage x 0,5 sur garde
	HitData.DamageType = Row.DamageType;
	HitData.bUnblockable = Row.bUnblockable;
	HitData.bUnparryable = !Row.bParryable;
	HitData.AttackTag = Row.AttackTag;
	return HitData;
}

void AEMBCreatureBase::ExecuteHit(FGameplayTag AttackTag, const int32 HitIndex)
{
	UWorld* World = GetWorld();
	if (!World || bIsDead || bIsStaggered || !HasAuthority())
	{
		return;
	}
	const FEMBCreatureAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row)
	{
		return;
	}

	const FEMBHitData Template = MakeHitData(*Row);
	const FVector Origin = GetActorLocation();
	const FVector Forward = GetActorForwardVector().GetSafeNormal2D();

	// Au plus 4 joueurs : un test géométrique direct est moins cher et plus fiable qu'une requête d'overlap.
	TArray<APawn*> Players;
	AEMBBossBase::CollectAlivePlayerPawns(World, Players);
	for (APawn* Pawn : Players)
	{
		if (!IsPawnInAttackShape(*Row, Origin, Forward, *Pawn))
		{
			continue;
		}

		FEMBHitData PerTarget = Template;
		PerTarget.HitLocation = Pawn->GetActorLocation();
		PerTarget.HitDirection = (Pawn->GetActorLocation() - Origin).GetSafeNormal();
		UEMBCombatComponent::ApplyHitToActor(Pawn, PerTarget);
	}
}

void AEMBCreatureBase::ExecuteLunge(FGameplayTag AttackTag, TWeakObjectPtr<AActor> TargetPtr)
{
	const FEMBCreatureAttackRow* Row = FindAttackRow(AttackTag);
	const AActor* Target = TargetPtr.Get();
	if (!HasAuthority() || bIsDead || bIsStaggered || !Row || !Target)
	{
		return;
	}

	// Vitesse déduite de la distance à parcourir et du temps restant avant l'impact (HitTimes[0]). Le frottement du
	// sol n'est pas modélisé : le point d'arrivée est à valider en jeu.
	const float ImpactTime = Row->HitTimes.Num() > 0 ? Row->HitTimes[0] : Row->TelegraphTime + 0.5f;
	const float TravelTime = FMath::Max(ImpactTime - Row->TelegraphTime, 0.2f);

	FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
	ToTarget.Z = 0.0;
	const float Distance = FMath::Min(static_cast<float>(ToTarget.Size()), Row->MaxRange > 0.f ? Row->MaxRange : static_cast<float>(ToTarget.Size()));
	const FVector Direction = ToTarget.GetSafeNormal();

	SetFacingLocked(true);
	LaunchCharacter(Direction * (Distance / TravelTime) + FVector(0.f, 0.f, EMBCreatureDefaults::LungeLift), true, false);
}

// ----------------------------------------------------------------------------------------------
// Réception des coups
// ----------------------------------------------------------------------------------------------

EEMBHitResult AEMBCreatureBase::ReceiveEMBHit(const FEMBHitData& Hit)
{
	// Les coups ne sont résolus que par le serveur.
	if (!HasAuthority() || bIsDead || !StatsComponent || Hit.Instigator == this)
	{
		return EEMBHitResult::Ignored;
	}

	float Damage = Hit.Damage;
	if (bIsStaggered && !Hit.bDamageOverTime)
	{
		Damage *= EMBCombatDefaults::CounterHitDamageMultiplier; // coup sur une créature étourdie
	}

	// Le contributeur est enregistré AVANT les dégâts : le coup fatal compte pour l'XP.
	if (Damage > 0.f)
	{
		RegisterContributor(Hit.Instigator);
	}

	if (Damage > 0.f)
	{
		StatsComponent->ApplyDamage(Damage, Hit.Instigator);
	}
	if (StatsComponent->IsDead())
	{
		return EEMBHitResult::Killed; // HandleStatsDeath a déjà été déclenché par OnDeath
	}

	// La rupture de poise déclenche HandlePoiseBroken via OnPoiseBroken.
	if (Hit.PoiseDamage > 0.f && !bIsStaggered)
	{
		StatsComponent->ApplyPoiseDamage(Hit.PoiseDamage, Hit.Instigator);
	}
	return EEMBHitResult::Hit;
}

void AEMBCreatureBase::RegisterContributor(AActor* Instigator)
{
	const UWorld* World = GetWorld();
	if (!World || !Instigator)
	{
		return;
	}

	// L'instigateur est le pawn du joueur ; pour un projectile ou une arme, on remonte à son instigateur.
	APawn* InstigatorPawn = Cast<APawn>(Instigator);
	if (!InstigatorPawn)
	{
		InstigatorPawn = Instigator->GetInstigator();
	}
	APlayerState* Player = InstigatorPawn ? InstigatorPawn->GetPlayerState() : nullptr;
	if (!Player)
	{
		return; // pas un joueur : aucun XP, aucune aggro
	}

	const double Now = World->GetTimeSeconds();
	Contributors.Add(Player, Now);
	LastAttacker = InstigatorPawn;
	LastAttackerTime = Now;

	// Purge des contributeurs oubliés (tableau de 4 joueurs au plus en pratique).
	for (auto It = Contributors.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || Now - It.Value() > EMBCreatureDefaults::ContributorMemory)
		{
			It.RemoveCurrent();
		}
	}
}

APawn* AEMBCreatureBase::GetRecentAttacker(const float MaxAge) const
{
	const UWorld* World = GetWorld();
	APawn* Attacker = LastAttacker.Get();
	if (!World || !Attacker || World->GetTimeSeconds() - LastAttackerTime > static_cast<double>(MaxAge))
	{
		return nullptr;
	}
	return Attacker;
}

bool AEMBCreatureBase::IsBelowFleeThreshold() const
{
	return bHasRow && CachedRow.FleeHealthFraction > 0.f && StatsComponent
		&& StatsComponent->GetHealthPercent() <= CachedRow.FleeHealthFraction;
}

void AEMBCreatureBase::SetRunning(const bool bRunning)
{
	if (!bHasRow)
	{
		return;
	}
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		const float Speed = bRunning ? CachedRow.RunSpeed : CachedRow.WalkSpeed;
		Move->MaxWalkSpeed = Speed;
		Move->MaxFlySpeed = Speed;
	}
}

// ----------------------------------------------------------------------------------------------
// Étourdissement
// ----------------------------------------------------------------------------------------------

void AEMBCreatureBase::HandlePoiseBroken(AActor* InInstigator)
{
	BeginStagger(EMBCombatDefaults::PoiseBreakStaggerDuration);
}

void AEMBCreatureBase::BeginStagger(const float Duration)
{
	if (!HasAuthority() || bIsDead || bIsStaggered)
	{
		return;
	}

	CancelAttack(true);
	bIsStaggered = true;

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
	}
	if (AAIController* AIController = Cast<AAIController>(GetController()))
	{
		AIController->StopMovement();
	}
	if (StaggerMontage)
	{
		Multicast_PlayMontage(StaggerMontage, 1.f);
	}

	GetWorldTimerManager().SetTimer(StaggerTimerHandle, this, &AEMBCreatureBase::EndStagger, Duration, false);
	PushStateToBlackboard();
}

void AEMBCreatureBase::EndStagger()
{
	bIsStaggered = false;
	PushStateToBlackboard();
}

void AEMBCreatureBase::PushStateToBlackboard()
{
	// Les états critiques sont poussés immédiatement : le BT abandonne ses tâches dès le changement d'état.
	if (AEMBCreatureAIController* AIController = Cast<AEMBCreatureAIController>(GetController()))
	{
		AIController->SyncBlackboardState();
	}
}

// ----------------------------------------------------------------------------------------------
// Mort
// ----------------------------------------------------------------------------------------------

void AEMBCreatureBase::HandleStatsDeath(AActor* Killer)
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	bIsDead = true; // répliqué : OnRep_IsDead chez les clients
	bIsStaggered = false;
	GetWorldTimerManager().ClearTimer(StaggerTimerHandle);
	CancelAttack(false);

	if (AController* AIController = GetController())
	{
		if (UBrainComponent* Brain = AIController->FindComponentByClass<UBrainComponent>())
		{
			Brain->StopLogic(TEXT("CreatureDead"));
		}
	}

	// Sort du budget serveur dès la mort (le cadavre ne compte plus), puis libère l'emplacement du spawner.
	if (UWorld* World = GetWorld())
	{
		if (UEMBCreatureSignificance* Significance = World->GetSubsystem<UEMBCreatureSignificance>())
		{
			Significance->UnregisterCreature(this);
		}
	}
	if (AEMBCreatureSpawner* Spawner = OwningSpawner.Get())
	{
		Spawner->NotifyChildDied(this);
	}

	AwardExperience();
	SpawnCarcass();
	PushStateToBlackboard();

	if (DeathMontage)
	{
		Multicast_StopMontages(0.1f);
		Multicast_PlayMontage(DeathMontage, 1.f);
	}
	ApplyDeathPresentation();

	OnCreatureDied.Broadcast(this, Killer);
	SetLifeSpan(DeathLifeSpan); // la carcasse (acteur de récolte) reste 600 s, indépendamment du cadavre
}

void AEMBCreatureBase::OnRep_IsDead()
{
	if (bIsDead)
	{
		ApplyDeathPresentation();
	}
}

void AEMBCreatureBase::ApplyDeathPresentation()
{
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->StopMovementImmediately();
		Move->DisableMovement();
	}
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	USkeletalMeshComponent* MeshComp = GetMesh();
	if (!MeshComp)
	{
		return;
	}

	// Un cadavre doit s'animer / tomber même s'il était dans un palier de détail réduit.
	MeshComp->SetComponentTickEnabled(true);
	MeshComp->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// Sans montage de mort : ragdoll. Jamais sur serveur dédié (aucun rendu, simulation inutile).
	if (!DeathMontage && !IsNetMode(NM_DedicatedServer))
	{
		MeshComp->SetCollisionProfileName(TEXT("Ragdoll"));
		MeshComp->SetSimulatePhysics(true);
	}
}

void AEMBCreatureBase::AwardExperience()
{
	const UWorld* World = GetWorld();
	if (!World || !bHasRow || CachedRow.XPReward <= 0)
	{
		return;
	}

	// XP : joueurs ayant infligé des dégâts dans les 30 dernières secondes et encore à <= 10 000 cm (§16).
	const double Now = World->GetTimeSeconds();
	const double RadiusSquared = FMath::Square(static_cast<double>(EMBCreatureDefaults::XPParticipantRadius));
	for (const TPair<TWeakObjectPtr<APlayerState>, double>& Pair : Contributors)
	{
		APlayerState* Player = Pair.Key.Get();
		if (!Player || Now - Pair.Value > static_cast<double>(EMBCreatureDefaults::ContributorMemory))
		{
			continue;
		}

		const APawn* Pawn = Player->GetPawn();
		if (!Pawn || FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation()) > RadiusSquared)
		{
			continue;
		}

		if (const AEMBPlayerState* EMBState = Cast<AEMBPlayerState>(Player))
		{
			if (UEMBProgressionComponent* Progression = EMBState->GetProgressionComponent())
			{
				Progression->AddXP(CachedRow.XPReward);
			}
		}
	}
}

void AEMBCreatureBase::SpawnCarcass()
{
	UWorld* World = GetWorld();
	if (!World || !bHasRow || CachedRow.CarcassNodeRow.IsNone())
	{
		return;
	}

	// Carcasse récoltable au couteau : nœud dynamique de Dev G, 600 s, non sauvegardé. Posée au sol sous la capsule.
	if (UEMBResourceNodeSubsystem* Nodes = World->GetSubsystem<UEMBResourceNodeSubsystem>())
	{
		const UCapsuleComponent* Capsule = GetCapsuleComponent();
		const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 0.f;
		const FTransform CarcassTransform(FRotator(0.f, GetActorRotation().Yaw, 0.f), GetActorLocation() - FVector(0.f, 0.f, HalfHeight));
		Nodes->SpawnDynamicNode(CachedRow.CarcassNodeRow, CarcassTransform, EMBCreatureDefaults::CarcassLifeSpan);
	}
}

// ----------------------------------------------------------------------------------------------
// Réseau : effets cosmétiques
// ----------------------------------------------------------------------------------------------

void AEMBCreatureBase::Multicast_PlayMontage_Implementation(UAnimMontage* Montage, const float PlayRate)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (AnimInstance && Montage)
	{
		AnimInstance->Montage_Play(Montage, PlayRate);
	}
}

void AEMBCreatureBase::Multicast_PlayAttackMontage_Implementation(UAnimMontage* Montage, const float PlayRate)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr;
	if (AnimInstance && Montage)
	{
		AnimInstance->Montage_Play(Montage, PlayRate);
	}
}

void AEMBCreatureBase::Multicast_StopMontages_Implementation(const float BlendOutTime)
{
	USkeletalMeshComponent* MeshComp = GetMesh();
	if (UAnimInstance* AnimInstance = MeshComp ? MeshComp->GetAnimInstance() : nullptr)
	{
		AnimInstance->StopAllMontages(BlendOutTime);
	}
}

void AEMBCreatureBase::Multicast_Telegraph_Implementation(FGameplayTag AttackTag, const float Duration)
{
	BP_OnAttackTelegraph(AttackTag, Duration);
}

// ----------------------------------------------------------------------------------------------
// Archétypes : seule la ligne de données change
// ----------------------------------------------------------------------------------------------

AEMBGhoulCreature::AEMBGhoulCreature()
{
	CreatureRowName = FName(TEXT("Ghoul"));
}

AEMBRockWyvernCreature::AEMBRockWyvernCreature()
{
	CreatureRowName = FName(TEXT("RockWyvern"));
}
