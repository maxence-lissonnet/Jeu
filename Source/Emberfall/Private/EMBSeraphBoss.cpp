// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBSeraphBoss.h"

#include "Emberfall.h"
#include "EMBBossAIController.h"
#include "EMBBossTags.h"
#include "EMBCombatComponent.h"
#include "EMBGameplayTags.h"
#include "EMBProjectile.h"
#include "EMBSeraphShadow.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"

namespace
{
	/** Repli de l'origine des projectiles (sans socket) : devant et légèrement au-dessus du boss (cm). */
	constexpr float FallbackProjectileForward = 150.f;
	constexpr float FallbackProjectileUp = 60.f;

	/** Portée de repli de la visée de la lance sans cible (cm). */
	constexpr float FallbackAimDistance = 2000.f;

	/** Ouverture de l'éventail de plumes par défaut si la ligne de la table n'en donne pas (degrés, demi-angle). */
	constexpr float DefaultFeatherHalfAngle = 40.f;
}

AEMBSeraphBoss::AEMBSeraphBoss()
{
	// PV 24 000, posture 700, régénération 50/s après 2,5 s (§11.3). Le contrôleur par défaut (AEMBBossAIController)
	// est celui de la base : le Blueprint de Nyxariel y assigne BT_Seraph et BB_Seraph.
	ConfigureBossStats(EMBSeraphDefaults::MaxHealthSolo, EMBSeraphDefaults::MaxPoise,
		EMBSeraphDefaults::PoiseRegenRate, EMBSeraphDefaults::PoiseRegenDelay);

	// Trois phases : P2 à 60 %, P3 à 25 % ; transitions invulnérables de 3 s ; étourdi 3 s.
	PhaseHealthThresholds = { EMBSeraphDefaults::Phase2HealthThreshold, EMBSeraphDefaults::Phase3HealthThreshold };
	PhaseTransitionDuration = EMBSeraphDefaults::PhaseTransitionDuration;
	StunDuration = EMBSeraphDefaults::StunDuration;

	// Pas d'enragé de la base : la "vitesse x1,2" de la phase 3 est une vitesse de déplacement (OnPhaseEnteredServer),
	// les durées de télégraphe explicites du référentiel (0,8 s, 0,9 s...) restent exactes.
	EnrageFromPhase = 0;

	// Résistances : physique 0 %, feu -25 % (faiblesse), corruption 60 %.
	DamageResistances.Add(EEMBDamageType::Physical, EMBSeraphDefaults::ResistPhysical);
	DamageResistances.Add(EEMBDamageType::Fire, EMBSeraphDefaults::ResistFire);
	DamageResistances.Add(EEMBDamageType::Corruption, EMBSeraphDefaults::ResistCorruption);

	// Après la Chute de l'étoile morte : vulnérable 4 s (bonus de dégâts, comme le dragon après son piqué).
	VulnerableDamageMultiplier = EMBSeraphDefaults::VulnerableDamageMultiplier;

	// Rayon de l'arène (propriété de la base) : le clocher est plus petit que la caldera du dragon.
	ArenaRadius = EMBSeraphDefaults::ArenaRadius;

	ProjectileClass = AEMBProjectile::StaticClass();
	ShadowClass = AEMBSeraphShadow::StaticClass();

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxFlySpeed = EMBSeraphDefaults::FlySpeed;
		Move->MaxWalkSpeed = EMBSeraphDefaults::GroundWalkSpeed;
		Move->RotationRate = FRotator(0.f, EMBSeraphDefaults::RotationRateYaw, 0.f);
	}
}

void AEMBSeraphBoss::BeginPlay()
{
	Super::BeginPlay();

	// Un boss déjà vaincu (sauvegarde) reste dormant : inutile de créer son pool.
	if (HasAuthority() && !IsDead())
	{
		PrewarmProjectilePool(EMBSeraphDefaults::ProjectilePoolPrewarm);
	}

	// Pool libéré à la fin de la rencontre (mort au combat OU boss déjà vaincu d'après une sauvegarde chargée
	// tardivement : OnEncounterEnded est diffusé dans les deux cas), et agrandi pendant la transition vers la phase 2.
	if (HasAuthority())
	{
		OnPhaseTransitionStarted.AddUniqueDynamic(this, &AEMBSeraphBoss::HandlePhaseTransitionStarted);
		OnEncounterEnded.AddUniqueDynamic(this, &AEMBSeraphBoss::HandleEncounterEnded);
	}
}

void AEMBSeraphBoss::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority())
	{
		DestroyProjectilePool();
		for (const TWeakObjectPtr<AEMBSeraphShadow>& Shadow : Shadows)
		{
			if (AEMBSeraphShadow* ShadowPtr = Shadow.Get())
			{
				ShadowPtr->Destroy();
			}
		}
		Shadows.Reset();
	}

	Super::EndPlay(EndPlayReason);
}

FGameplayTag AEMBSeraphBoss::GetDefeatedEventTag() const
{
	return EMBTags::Event_Boss_Killed_Seraph.GetTag();
}

bool AEMBSeraphBoss::ShouldFly() const
{
	return !IsDead() && GetCurrentPhase() < 3;
}

bool AEMBSeraphBoss::IsAttackAllowed(const FEMBBossAttackRow& Row) const
{
	// Phases 1-2 : aucune attaque depuis le sol (après un étourdissement ou une transition de phase, par exemple).
	// La sélection échoue, l'arbre bascule sur le repositionnement, qui refait décoller Nyxariel.
	if (ShouldFly() && !IsFlying())
	{
		return false;
	}

	// Ombres jumelles : pas de nouvelle invocation tant que des clones existent.
	if (Row.AttackTag == EMBBossTags::Attack_Seraph_TwinShadows.GetTag())
	{
		return GetAliveShadowCount() == 0;
	}
	return true;
}

// ----------------------------------------------------------------------------------------------
// Contexte
// ----------------------------------------------------------------------------------------------

AActor* AEMBSeraphBoss::ResolveCombatTarget() const
{
	if (const AEMBBossAIController* BossController = Cast<AEMBBossAIController>(GetController()))
	{
		if (AActor* Target = BossController->GetCombatTarget())
		{
			return Target;
		}
	}

	// Repli : joueur vivant le plus proche.
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(GetWorld(), Players);

	AActor* Nearest = nullptr;
	double NearestDistanceSquared = TNumericLimits<double>::Max();
	for (APawn* Pawn : Players)
	{
		const double DistanceSquared = FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation());
		if (DistanceSquared < NearestDistanceSquared)
		{
			NearestDistanceSquared = DistanceSquared;
			Nearest = Pawn;
		}
	}
	return Nearest;
}

float AEMBSeraphBoss::RollFeintDelay()
{
	// FRandomStream du boss (serveur) : le délai n'est jamais tiré côté client, il ne peut pas être anticipé.
	return RandomStream.FRandRange(EMBSeraphDefaults::FeintMinDelay, EMBSeraphDefaults::FeintMaxDelay);
}

FVector AEMBSeraphBoss::PredictTargetGroundPoint(const AActor* Target, const float Lead) const
{
	if (!Target)
	{
		return ProjectToGround(GetActorLocation());
	}

	// La cible est supposée continuer sur sa trajectoire pendant Lead secondes (décalage plafonné) : le joueur
	// qui change de direction après l'apparition de l'ombre l'évite.
	const FVector Predicted = Target->GetActorLocation()
		+ (Target->GetVelocity() * Lead).GetClampedToMaxSize(EMBSeraphDefaults::MaxPredictionOffset);
	return ProjectToGround(Predicted);
}

FVector AEMBSeraphBoss::GetProjectileOrigin() const
{
	const USkeletalMeshComponent* MeshComp = GetMesh();
	if (MeshComp && !ProjectileSocketName.IsNone() && MeshComp->DoesSocketExist(ProjectileSocketName))
	{
		return MeshComp->GetSocketLocation(ProjectileSocketName);
	}
	return GetActorLocation() + GetActorForwardVector() * FallbackProjectileForward + FVector(0.f, 0.f, FallbackProjectileUp);
}

// ----------------------------------------------------------------------------------------------
// Coups
// ----------------------------------------------------------------------------------------------

FEMBHitData AEMBSeraphBoss::MakeSeraphHitData(const FEMBBossAttackRow& Row, const float DamageScale)
{
	FEMBHitData Hit = MakeHitData(Row);
	Hit.Damage *= DamageScale;
	Hit.PoiseDamage *= DamageScale;
	Hit.StaminaDamage *= DamageScale;

	// Toute attaque d'un clone (30 % des dégâts) est non parable : une parade sur un clone ne doit rien rapporter.
	if (DamageScale < 1.f)
	{
		Hit.bUnparryable = true;
	}

	// Corruption propre à l'attaque (§11.3) : la table n'a pas de colonne pour elle.
	if (Row.AttackTag == EMBBossTags::Attack_Seraph_FeatherRain.GetTag())
	{
		Hit.CorruptionAmount = EMBSeraphDefaults::FeatherCorruption * DamageScale;
	}
	else if (Row.AttackTag == EMBBossTags::Attack_Seraph_BlackHalo.GetTag())
	{
		Hit.CorruptionAmount = EMBSeraphDefaults::HaloCorruption * DamageScale;
	}
	return Hit;
}

bool AEMBSeraphBoss::ResolveDashContact(const FVector& From, const FVector& To, const float Radius, const FEMBHitData& HitTemplate, TSet<TWeakObjectPtr<AActor>>& InOutAlreadyHit)
{
	if (!HasAuthority() || !GetWorld())
	{
		return false;
	}

	TArray<APawn*> Players;
	CollectAlivePlayerPawns(GetWorld(), Players);

	const FVector DashDirection = (To - From).GetSafeNormal();
	bool bParried = false;

	for (APawn* Pawn : Players)
	{
		if (Pawn == this)
		{
			continue;
		}

		const double Distance = FMath::PointDistToSegment(Pawn->GetActorLocation(), From, To);
		if (Distance - static_cast<double>(Pawn->GetSimpleCollisionRadius()) > static_cast<double>(Radius))
		{
			continue;
		}

		// Une seule résolution par joueur et par dash, qu'il soit touché ou qu'il esquive.
		bool bAlreadyInSet = false;
		InOutAlreadyHit.Add(Pawn, &bAlreadyInSet);
		if (bAlreadyInSet)
		{
			continue;
		}

		FEMBHitData Hit = HitTemplate;
		Hit.Instigator = this;
		Hit.HitLocation = Pawn->GetActorLocation();
		Hit.HitDirection = DashDirection;
		if (UEMBCombatComponent::ApplyHitToActor(Pawn, Hit) == EEMBHitResult::Parried)
		{
			bParried = true;
		}
	}
	return bParried;
}

// ----------------------------------------------------------------------------------------------
// Hooks de la base
// ----------------------------------------------------------------------------------------------

bool AEMBSeraphBoss::ScheduleCustomEvents(const FEMBBossAttackRow& Row, const float TimeScale, float& InOutLastEventTime)
{
	const FGameplayTag Tag = Row.AttackTag;

	if (Tag == EMBBossTags::Attack_Seraph_FeatherRain.GetTag())
	{
		// 0,9 s de télégraphe en vol stationnaire, puis l'éventail part d'un coup.
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBSeraphBoss::ExecuteFeatherRain, Tag), Row.TelegraphTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, Row.TelegraphTime);
		return true;
	}

	if (Tag == EMBBossTags::Attack_Seraph_LanceThrow.GetTag())
	{
		// La visée est figée un court instant avant le jet : la direction lue par le joueur est celle du tir.
		const float LockTime = FMath::Max(0.05f, Row.TelegraphTime - EMBSeraphDefaults::LanceAimLockLead);
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBSeraphBoss::LockLanceAim), LockTime * TimeScale);
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBSeraphBoss::ExecuteLanceThrow, Tag), Row.TelegraphTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, Row.TelegraphTime);
		return true;
	}

	if (Tag == EMBBossTags::Attack_Seraph_BlackHalo.GetTag())
	{
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBSeraphBoss::ExecuteBlackHalo, Tag), Row.TelegraphTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, Row.TelegraphTime);
		return true;
	}

	if (Tag == EMBBossTags::Attack_Seraph_TwinShadows.GetTag())
	{
		AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBSeraphBoss::ExecuteSummonShadows, Tag), Row.TelegraphTime * TimeScale);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, Row.TelegraphTime);
		return true;
	}

	// Ciseaux d'ailes : impacts génériques de la base (HitTimes 0,40 et 0,75 s). Estoc, Feinte et plongeons :
	// tâches du Behavior Tree.
	return false;
}

void AEMBSeraphBoss::OnTimedHitExecuted(const FEMBBossAttackRow& Row, const int32 HitIndex, const TArray<AActor*>& Landed)
{
	// Ciseaux d'ailes : chaque coup est rejoué par les clones, 0,3 s plus tard, sur la position actuelle de la cible.
	if (Row.AttackTag == EMBBossTags::Attack_Seraph_WingScissors.GetTag())
	{
		const AActor* Target = ResolveCombatTarget();
		FEMBSeraphEcho Echo;
		Echo.AttackTag = Row.AttackTag;
		Echo.EventIndex = HitIndex;
		Echo.AimPoint = Target ? Target->GetActorLocation() : GetActorLocation() + GetActorForwardVector() * FallbackAimDistance;
		BroadcastShadowEcho(Echo);
	}
}

void AEMBSeraphBoss::OnPhaseEnteredServer(const int32 NewPhase, const int32 OldPhase)
{
	if (NewPhase >= 3)
	{
		// Ailes brisées : au sol définitivement (la base l'a déjà posé pendant la transition), vitesse x1,2.
		SetFlying(false);
		if (UCharacterMovementComponent* Move = GetCharacterMovement())
		{
			Move->MaxWalkSpeed = EMBSeraphDefaults::GroundWalkSpeed * EMBSeraphDefaults::Phase3SpeedMultiplier;
		}
	}
}

void AEMBSeraphBoss::OnEncounterReset()
{
	// Après un wipe : plus de clones, projectiles rendus au pool, attaque en cours annulée (orientation, timers).
	DissipateAllShadows();
	for (AEMBProjectile* Projectile : ProjectilePool)
	{
		if (Projectile)
		{
			Projectile->ReturnToPool();
		}
	}
	CancelTimedAttack(true);
}

void AEMBSeraphBoss::OnDefeatedServer()
{
	DissipateAllShadows();
	for (AEMBProjectile* Projectile : ProjectilePool)
	{
		if (Projectile)
		{
			Projectile->ReturnToPool();
		}
	}
}

// ----------------------------------------------------------------------------------------------
// Pluie de plumes, Lance jetée, Halo noir, Ombres jumelles
// ----------------------------------------------------------------------------------------------

void AEMBSeraphBoss::ExecuteFeatherRain(FGameplayTag AttackTag)
{
	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row || IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	const AActor* Target = ResolveCombatTarget();
	const FVector Origin = GetProjectileOrigin();
	const FVector AimPoint = Target ? Target->GetActorLocation() : Origin + GetActorForwardVector() * FallbackAimDistance;

	LaunchFeatherFan(*Row, Origin, AimPoint, 1.f);

	FEMBSeraphEcho Echo;
	Echo.AttackTag = AttackTag;
	Echo.AimPoint = AimPoint;
	BroadcastShadowEcho(Echo);
}

void AEMBSeraphBoss::LockLanceAim()
{
	const AActor* Target = ResolveCombatTarget();
	LockedLanceAimPoint = Target ? Target->GetActorLocation() : GetActorLocation() + GetActorForwardVector() * FallbackAimDistance;
	SetFacingLocked(true);
}

void AEMBSeraphBoss::ExecuteLanceThrow(FGameplayTag AttackTag)
{
	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row || IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	LaunchLance(*Row, GetProjectileOrigin(), LockedLanceAimPoint, 1.f);

	FEMBSeraphEcho Echo;
	Echo.AttackTag = AttackTag;
	Echo.AimPoint = LockedLanceAimPoint;
	BroadcastShadowEcho(Echo);
}

void AEMBSeraphBoss::ExecuteBlackHalo(FGameplayTag AttackTag)
{
	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	if (!Row || IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	// Sphère de rayon 600 centrée sur le boss : seule la distance compte (ni angle, ni ligne de vue).
	const FVector Center = GetActorLocation();
	const float Radius = Row->EffectRadius > 0.f ? Row->EffectRadius : Row->MaxRange;
	ApplyAreaDamage(Center, Radius, MakeSeraphHitData(*Row));

	Multicast_ShowGroundMarker(ProjectToGround(Center), Radius, EMBSeraphDefaults::HaloFlashDuration, true);
	if (UNiagaraSystem* System = ResolveVFX(Row->VFX))
	{
		Multicast_SpawnVFXAtLocation(System, Center, FRotator::ZeroRotator, FVector::OneVector);
	}

	FEMBSeraphEcho Echo;
	Echo.AttackTag = AttackTag;
	Echo.AimPoint = Center;
	BroadcastShadowEcho(Echo);
}

void AEMBSeraphBoss::ExecuteSummonShadows(FGameplayTag AttackTag)
{
	UWorld* World = GetWorld();
	if (!World || !ShadowClass || IsDead() || IsStunned() || IsTransitioning() || GetAliveShadowCount() > 0)
	{
		return;
	}

	Shadows.RemoveAll([](const TWeakObjectPtr<AEMBSeraphShadow>& Shadow) { return !Shadow.IsValid(); });

	// Deux clones de part et d'autre du boss, à la même altitude. Durée de vie (20 s) et dissipation en un coup :
	// gérées par le clone lui-même.
	const FVector Right = GetActorRightVector();
	for (int32 Index = 0; Index < EMBSeraphDefaults::ShadowCount; ++Index)
	{
		const float Side = (Index % 2 == 0) ? 1.f : -1.f;
		const FVector Location = GetActorLocation() + Right * (Side * EMBSeraphDefaults::ShadowSideOffset);

		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		SpawnParams.Instigator = this;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		if (AEMBSeraphShadow* Shadow = World->SpawnActor<AEMBSeraphShadow>(ShadowClass, Location, GetActorRotation(), SpawnParams))
		{
			Shadows.Add(Shadow);
		}
	}
}

// ----------------------------------------------------------------------------------------------
// Projectiles
// ----------------------------------------------------------------------------------------------

void AEMBSeraphBoss::LaunchFeatherFan(const FEMBBossAttackRow& Row, const FVector& Origin, const FVector& AimPoint, const float DamageScale)
{
	if (!HasAuthority())
	{
		return;
	}

	// Rangées de 7, 9 et 8 plumes (haut, milieu, bas) : la rangée du milieu, à la hauteur de visée, a une plume
	// centrale (colonne d'indice 4 sur 9) : plus de zone d'impunité dans l'axe du tir. Total 24.
	constexpr int32 Rows = EMBSeraphDefaults::FeatherRows;
	constexpr int32 RowCounts[Rows] = { 7, 9, 8 };
	static_assert(RowCounts[0] + RowCounts[1] + RowCounts[2] == EMBSeraphDefaults::FeatherCount, "Les rangees doivent totaliser FeatherCount.");

	FVector BaseDirection = (AimPoint - Origin).GetSafeNormal();
	if (BaseDirection.IsNearlyZero())
	{
		BaseDirection = GetActorForwardVector();
	}
	const FRotator BaseRotation = BaseDirection.Rotation();
	const float HalfAngle = (Row.HalfAngleDeg > 0.f && Row.HalfAngleDeg < 179.f) ? Row.HalfAngleDeg : DefaultFeatherHalfAngle;
	const FEMBHitData Hit = MakeSeraphHitData(Row, DamageScale);

	for (int32 RowIndex = 0; RowIndex < Rows; ++RowIndex)
	{
		const float PitchOffset = (static_cast<float>(RowIndex) - static_cast<float>(Rows - 1) * 0.5f) * EMBSeraphDefaults::FeatherRowPitchStepDeg;
		const int32 Columns = RowCounts[RowIndex];

		for (int32 ColumnIndex = 0; ColumnIndex < Columns; ++ColumnIndex)
		{
			AEMBProjectile* Projectile = AcquireProjectile();
			if (!Projectile)
			{
				UE_LOG(LogEmberfall, Verbose, TEXT("%s : pool de projectiles epuise, plumes restantes ignorees."), *GetName());
				return;
			}

			const float YawAlpha = static_cast<float>(ColumnIndex) / static_cast<float>(Columns - 1);
			FRotator Rotation = BaseRotation;
			Rotation.Yaw += FMath::Lerp(-HalfAngle, HalfAngle, YawAlpha);
			Rotation.Pitch += PitchOffset;

			FEMBProjectileLaunchParams Params;
			Params.Origin = Origin;
			Params.Direction = Rotation.Vector();
			Params.Speed = EMBSeraphDefaults::FeatherSpeed;
			Params.CollisionRadius = EMBSeraphDefaults::FeatherRadius;
			Params.Lifetime = EMBSeraphDefaults::FeatherLifetime;
			Params.Style = EEMBProjectileStyle::Feather;
			Params.Hit = Hit;
			Projectile->Launch(Params);
		}
	}
}

void AEMBSeraphBoss::LaunchLance(const FEMBBossAttackRow& Row, const FVector& Origin, const FVector& AimPoint, const float DamageScale)
{
	if (!HasAuthority())
	{
		return;
	}

	AEMBProjectile* Projectile = AcquireProjectile();
	if (!Projectile)
	{
		return;
	}

	FEMBHitData Hit = MakeSeraphHitData(Row, DamageScale);
	// La lance d'un clone (30 % des dégâts) ne se renvoie pas : sinon une parade sur un clone infligerait 300 au boss.
	const bool bFullStrength = DamageScale >= 1.f;
	if (!bFullStrength)
	{
		Hit.bUnparryable = true;
	}

	FEMBProjectileLaunchParams Params;
	Params.Origin = Origin;
	Params.Direction = (AimPoint - Origin).GetSafeNormal();
	if (Params.Direction.IsNearlyZero())
	{
		Params.Direction = GetActorForwardVector();
	}
	Params.Speed = EMBSeraphDefaults::LanceSpeed;
	Params.CollisionRadius = EMBSeraphDefaults::LanceRadius;
	Params.Lifetime = EMBSeraphDefaults::LanceLifetime;
	Params.Style = EEMBProjectileStyle::Lance;
	Params.Hit = Hit;
	Params.bReflectOnPerfectParry = bFullStrength && Row.bParryable;
	Params.ReflectDamage = EMBSeraphDefaults::LanceReflectDamage;
	Params.ReflectSpeed = EMBSeraphDefaults::LanceReflectSpeed;
	Projectile->Launch(Params);
}

AEMBProjectile* AEMBSeraphBoss::AcquireProjectile()
{
	// Entrées nulles (projectile détruit par ailleurs) retirées au passage.
	ProjectilePool.RemoveAll([](const TObjectPtr<AEMBProjectile>& Entry) { return !IsValid(Entry); });

	for (AEMBProjectile* Candidate : ProjectilePool)
	{
		if (!Candidate->IsProjectileActive())
		{
			return Candidate;
		}
	}

	if (ProjectilePool.Num() >= EMBSeraphDefaults::ProjectilePoolMax)
	{
		return nullptr;
	}
	return SpawnPooledProjectile();
}

AEMBProjectile* AEMBSeraphBoss::SpawnPooledProjectile()
{
	UWorld* World = GetWorld();
	if (!World || !ProjectileClass)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.Instigator = this;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AEMBProjectile* Projectile = World->SpawnActor<AEMBProjectile>(ProjectileClass, GetActorLocation(), FRotator::ZeroRotator, SpawnParams);
	if (Projectile)
	{
		ProjectilePool.Add(Projectile);
	}
	return Projectile;
}

void AEMBSeraphBoss::PrewarmProjectilePool(const int32 TargetCount)
{
	// Amène le pool à TargetCount instances (au plus ProjectilePoolMax).
	ProjectilePool.RemoveAll([](const TObjectPtr<AEMBProjectile>& Entry) { return !IsValid(Entry); });
	while (ProjectilePool.Num() < FMath::Min(TargetCount, EMBSeraphDefaults::ProjectilePoolMax))
	{
		if (!SpawnPooledProjectile())
		{
			break;
		}
	}
}

void AEMBSeraphBoss::HandlePhaseTransitionStarted(const int32 PendingPhase)
{
	// Transition vers la phase 2 (3 s d'invulnérabilité) : le pool passe à 72 maintenant, hors combat, pour que les
	// volées de plumes des clones ne créent aucun acteur en pleine action.
	if (HasAuthority() && PendingPhase == 2)
	{
		PrewarmProjectilePool(EMBSeraphDefaults::ProjectilePoolPhase2Prewarm);
	}
}

void AEMBSeraphBoss::HandleEncounterEnded()
{
	if (!HasAuthority())
	{
		return;
	}
	DissipateAllShadows();
	DestroyProjectilePool();
}

void AEMBSeraphBoss::DestroyProjectilePool()
{
	for (AEMBProjectile* Projectile : ProjectilePool)
	{
		if (IsValid(Projectile))
		{
			Projectile->Destroy();
		}
	}
	ProjectilePool.Reset();
}

// ----------------------------------------------------------------------------------------------
// Ombres jumelles
// ----------------------------------------------------------------------------------------------

void AEMBSeraphBoss::BroadcastShadowEcho(const FEMBSeraphEcho& Echo)
{
	if (!HasAuthority())
	{
		return;
	}
	for (const TWeakObjectPtr<AEMBSeraphShadow>& Shadow : Shadows)
	{
		if (AEMBSeraphShadow* ShadowPtr = Shadow.Get())
		{
			ShadowPtr->QueueEcho(Echo);
		}
	}
}

int32 AEMBSeraphBoss::GetAliveShadowCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AEMBSeraphShadow>& Shadow : Shadows)
	{
		const AEMBSeraphShadow* ShadowPtr = Shadow.Get();
		if (ShadowPtr && !ShadowPtr->IsDissipated())
		{
			++Count;
		}
	}
	return Count;
}

void AEMBSeraphBoss::DissipateAllShadows()
{
	for (const TWeakObjectPtr<AEMBSeraphShadow>& Shadow : Shadows)
	{
		if (AEMBSeraphShadow* ShadowPtr = Shadow.Get())
		{
			ShadowPtr->Dissipate();
		}
	}
}

// ----------------------------------------------------------------------------------------------
// Cosmétique
// ----------------------------------------------------------------------------------------------

void AEMBSeraphBoss::Multicast_TipFlash_Implementation(const FGameplayTag AttackTag, const float Lead)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnTipFlash(AttackTag, Lead);
	}
}
