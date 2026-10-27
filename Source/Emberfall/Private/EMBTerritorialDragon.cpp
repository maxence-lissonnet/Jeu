// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBTerritorialDragon.h"

#include "Emberfall.h"
#include "EMBBossAIController.h"
#include "EMBCombatComponent.h"
#include "EMBCreatureSpawner.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBResourceNodeSubsystem.h"
#include "EMBStatsComponent.h"

#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

const FName AEMBTerritorialDragon::BBKey_NestLocation(TEXT("NestLocation"));
const FName AEMBTerritorialDragon::BBKey_TerritoryAggressive(TEXT("bTerritoryAggressive"));
const FName AEMBTerritorialDragon::BBKey_ReturningToNest(TEXT("bReturningToNest"));

AEMBTerritorialDragon::AEMBTerritorialDragon()
{
	// PV 25 000, posture 800 (§16). Valeurs de repli : PreInitializeComponents les remplace par la ligne "Sylvadrac"
	// de DT_Creatures. Régénération de posture : (*) 30/s après 3 s.
	ConfigureBossStats(EMBSylvadracDefaults::MaxHealthSolo, EMBSylvadracDefaults::MaxPoise,
		EMBSylvadracDefaults::PoiseRegenRate, EMBSylvadracDefaults::PoiseRegenDelay);

	// Une seule phase (aucun seuil), pas d'enragé. Résistances : aucune (le §16 n'en donne pas).
	PhaseHealthThresholds.Reset();

	// La rencontre couvre tout le territoire : ni abandon prématuré, ni sauvegarde reportée à tort hors du territoire.
	ArenaRadius = EMBSylvadracDefaults::TerritoryRadius;

	CreatureTable = TSoftObjectPtr<UDataTable>(EMBCreatureData::GetDefaultTablePath());

	// Lame des joueurs (EMBMeleeHit = ECC_GameTraceChannel1, Ignore par défaut) : mesh en Overlap, capsule en Ignore.
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		MeshComp->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Overlap);
	}
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Ignore);
	}

	// Hors combat : 10 Hz suffisent (la base fixe 30 Hz) ; EnterAggressive remonte à 30 Hz.
	SetNetUpdateFrequency(EMBSylvadracDefaults::IdleNetUpdateFrequency);

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = EMBSylvadracDefaults::WalkSpeed;
	}
}

void AEMBTerritorialDragon::PreInitializeComponents()
{
	Super::PreInitializeComponents();

	// Avant InitializeComponent du composant de stats (qui copie MaxPoise dans la jauge) et avant le BeginPlay de la
	// base (qui pose les PV depuis BaseMaxHealth) : la ligne de données est la source de vérité, le code le repli.
	bHasRow = EMBCreatureData::FindRow(CreatureTable, CreatureRowName, CachedRow);
	if (!bHasRow)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBTerritorialDragon %s : ligne '%s' introuvable, valeurs de repli."), *GetName(), *CreatureRowName.ToString());
		return;
	}

	ConfigureBossStats(CachedRow.MaxHealth, CachedRow.MaxPoise, EMBSylvadracDefaults::PoiseRegenRate, EMBSylvadracDefaults::PoiseRegenDelay);
	TerritoryRadius = FMath::Max(CachedRow.LeashRadius, 1000.f);
}

void AEMBTerritorialDragon::BeginPlay()
{
	Super::BeginPlay();

	NestLocation = GetActorLocation();
	NestRotation = GetActorRotation();
	ArenaRadius = TerritoryRadius;

	// Aucun Tick : la machine d'états du territoire tourne à 2 Hz, serveur uniquement.
	if (HasAuthority() && !IsDead())
	{
		ApplyTerritorySpeed();
		PushTerritoryToBlackboard();
		GetWorldTimerManager().SetTimer(TerritoryTimerHandle, this, &AEMBTerritorialDragon::TickTerritory,
			EMBSylvadracDefaults::TerritoryTickInterval, true, EMBSylvadracDefaults::TerritoryTickInterval);
	}
}

FGameplayTag AEMBTerritorialDragon::GetDefeatedEventTag() const
{
	// Volontairement invalide : le Sylvadrac réapparaît, il ne doit pas devenir un "boss vaincu" définitif.
	return FGameplayTag();
}

int64 AEMBTerritorialDragon::GetDefeatXP() const
{
	return bHasRow ? static_cast<int64>(CachedRow.XPReward) : EMBSylvadracDefaults::XPReward;
}

void AEMBTerritorialDragon::SetOwningSpawner(AEMBCreatureSpawner* Spawner)
{
	OwningSpawner = Spawner;
}

AActor* AEMBTerritorialDragon::GetCombatTargetActor() const
{
	const AEMBBossAIController* BossController = Cast<AEMBBossAIController>(GetController());
	return BossController ? BossController->GetCombatTarget() : nullptr;
}

float AEMBTerritorialDragon::PlanarDistanceToNest(const FVector& Location) const
{
	return static_cast<float>(FVector::Dist2D(Location, NestLocation));
}

// ----------------------------------------------------------------------------------------------
// Territoire
// ----------------------------------------------------------------------------------------------

void AEMBTerritorialDragon::TickTerritory()
{
	UWorld* World = GetWorld();
	if (!World || IsDead())
	{
		GetWorldTimerManager().ClearTimer(TerritoryTimerHandle);
		return;
	}

	const double Now = World->GetTimeSeconds();

	// Joueurs vivants dans le territoire, et le plus proche du nid.
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(World, Players);
	int32 PlayersInTerritory = 0;
	APawn* NearestPlayer = nullptr;
	float NearestDistance = TNumericLimits<float>::Max();
	for (APawn* Player : Players)
	{
		const float Distance = PlanarDistanceToNest(Player->GetActorLocation());
		if (Distance <= TerritoryRadius)
		{
			++PlayersInTerritory;
			if (Distance < NearestDistance)
			{
				NearestDistance = Distance;
				NearestPlayer = Player;
			}
		}
	}

	switch (TerritoryState)
	{
	case EEMBTerritoryState::Idle:
		if (PlayersInTerritory > 0)
		{
			BeginWarning(Now);
		}
		break;

	case EEMBTerritoryState::Warned:
		if (PlayersInTerritory == 0)
		{
			TerritoryState = EEMBTerritoryState::Idle; // le joueur est reparti avant la fin du décompte
		}
		else if (Now - WarnStartTime >= static_cast<double>(AggressionDelay))
		{
			EnterAggressive(NearestPlayer);
		}
		break;

	case EEMBTerritoryState::Aggressive:
		if (PlayersInTerritory > 0)
		{
			EmptySince = -1.0;
		}
		else if (EmptySince < 0.0)
		{
			EmptySince = Now;
		}
		else if (Now - EmptySince >= static_cast<double>(EMBSylvadracDefaults::ReturnDelay))
		{
			EnterReturning();
			break;
		}

		// Laisse : le dragon poursuivant un joueur ne s'éloigne pas indéfiniment de son nid.
		if (PlanarDistanceToNest(GetActorLocation()) > TerritoryRadius + EMBSylvadracDefaults::LeashMargin)
		{
			EnterReturning();
		}
		break;

	case EEMBTerritoryState::Returning:
		if (PlanarDistanceToNest(GetActorLocation()) <= EMBSylvadracDefaults::NestArrivalRadius)
		{
			TerritoryState = EEMBTerritoryState::Idle;
			ApplyTerritorySpeed();
		}
		break;
	}

	// Régénération hors combat (au nid, pendant l'avertissement et pendant le retour).
	if (TerritoryState != EEMBTerritoryState::Aggressive)
	{
		RegenerateHealth();
	}
	PushTerritoryToBlackboard();
}

void AEMBTerritorialDragon::BeginWarning(const double Now)
{
	TerritoryState = EEMBTerritoryState::Warned;
	WarnStartTime = Now;

	// Rugissement d'avertissement : une seule fois par RoarCooldown, même si le joueur entre et sort plusieurs fois.
	if (Now - LastRoarTime >= static_cast<double>(EMBSylvadracDefaults::RoarCooldown))
	{
		LastRoarTime = Now;
		PlayRoar();
	}
}

void AEMBTerritorialDragon::PlayRoar()
{
	if (RoarMontage)
	{
		PlayAttackMontage(RoarMontage, 1.f);
	}
	// Télégraphe cosmétique chez tous les clients (son de rugissement, secousse de caméra... côté Blueprint).
	Multicast_Telegraph(EMBCreatureTags::Attack_Sylvadrac_Roar.GetTag(), EMBSylvadracDefaults::RoarDuration);
}

void AEMBTerritorialDragon::EnterAggressive(AActor* Provoker)
{
	if (TerritoryState == EEMBTerritoryState::Aggressive || IsDead())
	{
		return;
	}

	TerritoryState = EEMBTerritoryState::Aggressive;
	EmptySince = -1.0;
	SetNetUpdateFrequency(EMBSylvadracDefaults::CombatNetUpdateFrequency);

	// Échelle coop figée ici au plus tard (idempotent) : joueurs vivants dans l'arène = le territoire.
	BeginEncounter();

	if (AEMBBossAIController* BossController = Cast<AEMBBossAIController>(GetController()))
	{
		if (!BossController->GetCombatTarget() && Cast<APawn>(Provoker))
		{
			BossController->SetCombatTarget(Provoker);
		}
	}

	ApplyTerritorySpeed();
	PushTerritoryToBlackboard();
}

void AEMBTerritorialDragon::EnterReturning()
{
	if (TerritoryState == EEMBTerritoryState::Returning || IsDead())
	{
		return;
	}

	TerritoryState = EEMBTerritoryState::Returning;
	EmptySince = -1.0;
	SetNetUpdateFrequency(EMBSylvadracDefaults::IdleNetUpdateFrequency);

	CancelTimedAttack(true);
	if (AEMBBossAIController* BossController = Cast<AEMBBossAIController>(GetController()))
	{
		BossController->SetCombatTarget(nullptr);
	}

	ApplyTerritorySpeed();
	PushTerritoryToBlackboard();
}

void AEMBTerritorialDragon::OnEncounterReset()
{
	// La base abandonne la rencontre après 30 s sans joueur vivant dans l'arène (PV remis au maximum) : le BT doit rentrer.
	if (TerritoryState == EEMBTerritoryState::Aggressive)
	{
		EnterReturning();
	}
}

void AEMBTerritorialDragon::RegenerateHealth()
{
	UEMBStatsComponent* Stats = GetStatsComponent();
	if (Stats && !Stats->IsDead() && Stats->GetHealth() < Stats->GetMaxHealth())
	{
		Stats->Heal(Stats->GetMaxHealth() * EMBSylvadracDefaults::RegenFractionPerSecond * EMBSylvadracDefaults::TerritoryTickInterval);
	}
}

void AEMBTerritorialDragon::ApplyTerritorySpeed()
{
	// Marche au nid ; course en combat et pendant le retour.
	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		const bool bRunning = TerritoryState == EEMBTerritoryState::Aggressive || TerritoryState == EEMBTerritoryState::Returning;
		const float Walk = bHasRow ? CachedRow.WalkSpeed : EMBSylvadracDefaults::WalkSpeed;
		const float Run = bHasRow ? CachedRow.RunSpeed : EMBSylvadracDefaults::RunSpeed;
		Move->MaxWalkSpeed = bRunning ? Run : Walk;
	}
}

void AEMBTerritorialDragon::PushTerritoryToBlackboard()
{
	// Clés propres au territoire. SetValue ne notifie les observateurs que si la valeur change : appel à chaque tick sans coût.
	AAIController* AIController = Cast<AAIController>(GetController());
	UBlackboardComponent* Blackboard = AIController ? AIController->GetBlackboardComponent() : nullptr;
	if (!Blackboard)
	{
		return;
	}
	Blackboard->SetValueAsBool(BBKey_TerritoryAggressive, TerritoryState == EEMBTerritoryState::Aggressive);
	Blackboard->SetValueAsBool(BBKey_ReturningToNest, TerritoryState == EEMBTerritoryState::Returning);
	Blackboard->SetValueAsVector(BBKey_NestLocation, NestLocation);
}

void AEMBTerritorialDragon::BeginEncounter()
{
	if (TerritoryState == EEMBTerritoryState::Aggressive)
	{
		Super::BeginEncounter();
	}
}

bool AEMBTerritorialDragon::IsEncounterActive() const
{
	return TerritoryState == EEMBTerritoryState::Aggressive && Super::IsEncounterActive();
}

EEMBHitResult AEMBTerritorialDragon::ReceiveEMBHit(const FEMBHitData& Hit)
{
	// Contributeurs de l'XP : dégâts > 0 (enregistré AVANT le coup, le coup fatal compte).
	if (HasAuthority() && !IsDead() && Hit.Damage > 0.f && GetWorld())
	{
		const APawn* Attacker = Cast<APawn>(Hit.Instigator.Get());
		if (!Attacker && Hit.Instigator)
		{
			Attacker = Hit.Instigator->GetInstigator();
		}
		if (APlayerState* Player = Attacker ? Attacker->GetPlayerState() : nullptr)
		{
			const double Now = GetWorld()->GetTimeSeconds();
			Contributors.Add(Player, Now);
			for (auto It = Contributors.CreateIterator(); It; ++It)
			{
				if (!It.Key().IsValid() || Now - It.Value() > static_cast<double>(EMBCreatureDefaults::ContributorMemory))
				{
					It.RemoveCurrent();
				}
			}
		}
	}

	// Provocation : un coup d'un joueur déclenche l'agression sans attendre les 20 s (ni le retour au nid).
	if (HasAuthority() && !IsDead() && Hit.Damage > 0.f && !Hit.bDamageOverTime
		&& TerritoryState != EEMBTerritoryState::Aggressive)
	{
		const APawn* Attacker = Cast<APawn>(Hit.Instigator.Get());
		if (Attacker && Attacker->IsPlayerControlled())
		{
			EnterAggressive(Hit.Instigator.Get());
		}
	}
	return Super::ReceiveEMBHit(Hit);
}

// ----------------------------------------------------------------------------------------------
// Attaques spéciales (piqué bas, nuage toxique) et poison
// ----------------------------------------------------------------------------------------------

bool AEMBTerritorialDragon::ScheduleCustomEvents(const FEMBBossAttackRow& Row, const float TimeScale, float& InOutLastEventTime)
{
	if (Row.AttackTag == EMBCreatureTags::Attack_Sylvadrac_LowDive.GetTag())
	{
		// Bond lancé à la fin du télégraphe, impact à HitTimes[0]. L'orientation reste libre jusqu'au bond (suivi de la cible).
		const float LaunchTime = Row.TelegraphTime;
		const float ImpactTime = Row.HitTimes.Num() > 0 ? Row.HitTimes[0] : LaunchTime + EMBSylvadracDefaults::LowDiveFlightTime;
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBTerritorialDragon::LaunchLowDive, Row.AttackTag, FMath::Max(ImpactTime - LaunchTime, 0.2f)), LaunchTime * TimeScale);
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBTerritorialDragon::ResolveLowDiveImpact, Row.AttackTag), ImpactTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, ImpactTime);
		return true;
	}

	if (Row.AttackTag == EMBCreatureTags::Attack_Sylvadrac_PoisonCloud.GetTag())
	{
		// Le point d'impact est annoncé tout de suite par un marqueur au sol (anticipation de la course de la cible), le nuage
		// apparaît à HitTimes[0] : c'est la fenêtre pour quitter la zone.
		const float DropTime = Row.HitTimes.Num() > 0 ? Row.HitTimes[0] : Row.TelegraphTime;
		const FVector Center = ComputeCloudCenter();
		const float Radius = Row.EffectRadius > 0.f ? Row.EffectRadius : EMBSylvadracDefaults::CloudRadius;
		Multicast_ShowGroundMarker(Center, Radius, DropTime * TimeScale, true);
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBTerritorialDragon::SpawnPoisonCloud, Row.AttackTag, Center), DropTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, DropTime);
		return true;
	}

	return false; // la morsure : impacts génériques de la base (HitTimes), puis OnTimedHitExecuted
}

void AEMBTerritorialDragon::OnTimedHitExecuted(const FEMBBossAttackRow& Row, const int32 HitIndex, const TArray<AActor*>& Landed)
{
	// La morsure empoisonne ceux qu'elle touche (un coup bloqué n'est pas dans Landed : pas de poison).
	if (Row.AttackTag == EMBCreatureTags::Attack_Sylvadrac_Bite.GetTag())
	{
		for (AActor* Victim : Landed)
		{
			ApplyPoison(Victim);
		}
	}
}

FVector AEMBTerritorialDragon::ComputeCloudCenter() const
{
	const AActor* Target = GetCombatTargetActor();
	if (!Target)
	{
		return ProjectToGround(GetActorLocation() + GetActorForwardVector().GetSafeNormal2D() * 1500.f);
	}

	// Anticipation de la course du joueur (plafonnée), comme la pluie de braises du dragon.
	const FVector Predicted = Target->GetActorLocation()
		+ (Target->GetVelocity() * EMBSylvadracDefaults::CloudPredictionTime).GetClampedToMaxSize(EMBSylvadracDefaults::CloudPredictionMax);
	return ProjectToGround(Predicted);
}

void AEMBTerritorialDragon::SpawnPoisonCloud(FGameplayTag AttackTag, FVector Center)
{
	const UWorld* World = GetWorld();
	if (!World || IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	FEMBPoisonCloud Cloud;
	Cloud.Center = Center;
	Cloud.Radius = (Row && Row->EffectRadius > 0.f) ? Row->EffectRadius : EMBSylvadracDefaults::CloudRadius;
	const float Duration = (Row && Row->ActiveDuration > 0.f) ? Row->ActiveDuration : EMBSylvadracDefaults::CloudDuration;
	Cloud.EndTime = World->GetTimeSeconds() + static_cast<double>(Duration);
	PoisonClouds.Add(Cloud);

	// Marqueur au sol pendant toute la durée du nuage, et VFX éventuel de la ligne (système Niagara à la durée du nuage).
	Multicast_ShowGroundMarker(Center, Cloud.Radius, Duration, false);
	if (Row)
	{
		if (UNiagaraSystem* VFX = ResolveVFX(Row->VFX))
		{
			Multicast_SpawnVFXAtLocation(VFX, Center, FRotator::ZeroRotator, FVector::OneVector);
		}
	}

	EnsurePoisonTimer();
}

void AEMBTerritorialDragon::ApplyPoison(AActor* Victim)
{
	const UWorld* World = GetWorld();
	if (!HasAuthority() || !Victim || !World)
	{
		return;
	}

	// Un seul timer pour toutes les victimes ; réappliquer rafraîchit la durée sans cumuler les dégâts.
	PoisonEndTimes.Add(Victim, World->GetTimeSeconds() + static_cast<double>(EMBSylvadracDefaults::PoisonDuration));
	EnsurePoisonTimer();
}

void AEMBTerritorialDragon::EnsurePoisonTimer()
{
	FTimerManager& TimerManager = GetWorldTimerManager();
	if (!TimerManager.IsTimerActive(PoisonTimerHandle))
	{
		TimerManager.SetTimer(PoisonTimerHandle, this, &AEMBTerritorialDragon::TickPoison, EMBSylvadracDefaults::PoisonTickInterval, true);
	}
}

void AEMBTerritorialDragon::TickPoison()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	// 1. Nuages : quiconque s'y tient est empoisonné (rafraîchi à chaque tick). Un nuage périmé disparaît.
	if (PoisonClouds.Num() > 0)
	{
		TArray<APawn*> Players;
		CollectAlivePlayerPawns(World, Players);
		for (int32 Index = PoisonClouds.Num() - 1; Index >= 0; --Index)
		{
			const FEMBPoisonCloud& Cloud = PoisonClouds[Index];
			if (Now >= Cloud.EndTime)
			{
				PoisonClouds.RemoveAtSwap(Index);
				continue;
			}
			for (APawn* Player : Players)
			{
				const double Distance = FVector::Dist(Player->GetActorLocation(), Cloud.Center) - static_cast<double>(Player->GetSimpleCollisionRadius());
				if (Distance <= static_cast<double>(Cloud.Radius))
				{
					double& EndTime = PoisonEndTimes.FindOrAdd(Player);
					EndTime = FMath::Max(EndTime, Now + static_cast<double>(EMBSylvadracDefaults::CloudPoisonRefresh));
				}
			}
		}
	}

	// 2. Dégâts continus : 20/s en ticks de 0,25 s, directs (ni esquive, ni garde, ni parade, ni poise).
	FEMBHitData PoisonHit;
	PoisonHit.Instigator = this;
	PoisonHit.Damage = EMBSylvadracDefaults::PoisonDps * EMBSylvadracDefaults::PoisonTickInterval;
	PoisonHit.PoiseDamage = 0.f;
	PoisonHit.DamageType = EEMBDamageType::Physical; // pas Corruption : la survie ajouterait de la Corruption à chaque tick
	PoisonHit.bUnblockable = true;
	PoisonHit.bUnparryable = true;
	PoisonHit.bDamageOverTime = true;
	PoisonHit.AttackTag = EMBCreatureTags::Attack_Sylvadrac_Poison.GetTag();

	for (auto It = PoisonEndTimes.CreateIterator(); It; ++It)
	{
		AActor* Victim = It.Key().Get();
		if (!Victim || Now >= It.Value())
		{
			It.RemoveCurrent();
			continue;
		}

		PoisonHit.HitLocation = Victim->GetActorLocation();
		PoisonHit.HitDirection = FVector::UpVector;
		const EEMBHitResult Result = UEMBCombatComponent::ApplyHitToActor(Victim, PoisonHit);
		if (Result == EEMBHitResult::Killed || Result == EEMBHitResult::Ignored)
		{
			It.RemoveCurrent(); // victime morte (ou non touchable) : le poison s'arrête
		}
	}

	if (PoisonEndTimes.Num() == 0 && PoisonClouds.Num() == 0)
	{
		World->GetTimerManager().ClearTimer(PoisonTimerHandle);
	}
}

void AEMBTerritorialDragon::LaunchLowDive(FGameplayTag AttackTag, const float FlightDuration)
{
	if (IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	const AActor* Target = GetCombatTargetActor();

	FVector Direction = GetActorForwardVector().GetSafeNormal2D();
	float Distance = EMBSylvadracDefaults::LowDiveDefaultDistance;
	if (Target)
	{
		FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
		ToTarget.Z = 0.0;
		Distance = static_cast<float>(ToTarget.Size());
		Direction = ToTarget.GetSafeNormal();
	}
	if (Row && Row->MaxRange > 0.f)
	{
		Distance = FMath::Min(Distance, Row->MaxRange);
	}

	// Cap figé au départ du bond : la trajectoire est lisible et esquivable. Vitesse déduite du temps de vol ; le
	// frottement du sol n'est pas modélisé (point d'arrivée à valider en jeu).
	SetFacingLocked(true);
	LaunchCharacter(Direction * (Distance / FMath::Max(FlightDuration, 0.2f)) + FVector(0.f, 0.f, EMBSylvadracDefaults::LowDiveLift), true, false);
}

void AEMBTerritorialDragon::ResolveLowDiveImpact(FGameplayTag AttackTag)
{
	if (IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}
	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row)
	{
		return;
	}

	const float Radius = Row->EffectRadius > 0.f ? Row->EffectRadius : 350.f;
	ApplyAreaDamage(GetActorLocation(), Radius, MakeHitData(*Row), false, nullptr, true);
	Multicast_ShowGroundMarker(ProjectToGround(GetActorLocation()), Radius, EMBSylvadracDefaults::LowDiveImpactFlash, true);
}

// ----------------------------------------------------------------------------------------------
// Mort et réapparition
// ----------------------------------------------------------------------------------------------

void AEMBTerritorialDragon::OnDefeatedServer()
{
	// Appelé par la base à la mort au combat, AVANT la diffusion de l'événement de défaite (tag invalide : sans effet).
	FTimerManager& TimerManager = GetWorldTimerManager();
	TimerManager.ClearTimer(TerritoryTimerHandle);
	TimerManager.ClearTimer(PoisonTimerHandle);
	PoisonEndTimes.Reset();
	PoisonClouds.Reset();

	AwardParticipantExperience();
	SpawnCarcass();
	ScheduleRespawn();

	// Le cadavre disparaît quand la carcasse (600 s) disparaît ; le spawner a déjà la réapparition en charge.
	SetLifeSpan(EMBCreatureDefaults::CarcassLifeSpan + 30.f);
}

void AEMBTerritorialDragon::AwardParticipantExperience()
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Participants : contributeurs (dégâts > 0 dans les 30 dernières secondes) encore à <= 10 000 cm, comme les créatures.
	// L'XP est versée à CHAQUE mort : ce n'est pas un événement monde, aucun filtre "première fois".
	const double Now = World->GetTimeSeconds();
	const double RadiusSquared = FMath::Square(static_cast<double>(EMBCreatureDefaults::XPParticipantRadius));
	const int64 XP = GetDefeatXP();

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

		const AEMBPlayerState* EMBState = Cast<AEMBPlayerState>(Player);
		if (UEMBProgressionComponent* Progression = EMBState ? EMBState->GetProgressionComponent() : nullptr)
		{
			Progression->AddXP(XP);
			Progression->GrantTrigger(EMBCreatureTags::Event_Creature_Killed_Sylvadrac.GetTag());
		}
	}
}

void AEMBTerritorialDragon::SpawnCarcass()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FName CarcassRow = (bHasRow && !CachedRow.CarcassNodeRow.IsNone()) ? CachedRow.CarcassNodeRow : FName(TEXT("SylvadracCarcass"));
	if (UEMBResourceNodeSubsystem* Nodes = World->GetSubsystem<UEMBResourceNodeSubsystem>())
	{
		const FTransform CarcassTransform(FRotator(0.f, GetActorRotation().Yaw, 0.f), ProjectToGround(GetActorLocation()));
		Nodes->SpawnDynamicNode(CarcassRow, CarcassTransform, EMBCreatureDefaults::CarcassLifeSpan);
	}
}

void AEMBTerritorialDragon::ScheduleRespawn()
{
	// Cas nominal : le dragon vient d'un spawner, qui compte les 144 min à partir de la mort.
	if (AEMBCreatureSpawner* Spawner = OwningSpawner.Get())
	{
		Spawner->NotifyChildDied(this, RespawnDelaySeconds);
		return;
	}

	// Dragon posé à la main : le cadavre sera détruit, il faut un objet qui survive pour porter le délai. On crée un
	// spawner au nid, qui recréera un dragon de la même classe après RespawnDelaySeconds. Non sauvegardé (v1.4).
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	AEMBCreatureSpawner* Spawner = World->SpawnActorDeferred<AEMBCreatureSpawner>(AEMBCreatureSpawner::StaticClass(),
		FTransform(NestRotation, NestLocation), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Spawner)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBTerritorialDragon %s : impossible de creer le spawner de reapparition."), *GetName());
		return;
	}
	Spawner->InitializeAsRespawner(GetClass(), RespawnDelaySeconds, TerritoryRadius, EMBSylvadracDefaults::FallbackSpawnerMinPlayerDistance);
	Spawner->FinishSpawning(FTransform(NestRotation, NestLocation));
}
