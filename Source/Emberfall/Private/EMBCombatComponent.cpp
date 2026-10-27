// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCombatComponent.h"
#include "Emberfall.h"
#include "EMBGameplayTags.h"
#include "EMBHitReceiver.h"
#include "EMBStatsComponent.h"
#include "GameplayTagAssetInterface.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "CollisionQueryParams.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Misc/GuardValue.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Marge ajoutée à la durée d'un montage pour le temporisateur de secours de fin d'action. */
	constexpr float MontageFailsafeMargin = 0.5f;

	/** Bornes du taux de lecture appliqué au montage d'étourdissement pour qu'il épouse la durée exacte. */
	constexpr float MinStaggerPlayRate = 0.25f;
	constexpr float MaxStaggerPlayRate = 4.f;
}

// ================================================================================================
// Construction / cycle de vie
// ================================================================================================

UEMBCombatComponent::UEMBCombatComponent()
{
	// FSM événementielle : aucun Tick. Tout passe par les RPC, les notifies, les montages et des temporisateurs.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);

	WeaponBaseDamage = EMBCombatDefaults::WeaponBaseDamage;
	GuardHalfAngleDegrees = 100.f;
	ParryLatencyTolerance = 0.f;
	InputBufferTime = EMBCombatDefaults::InputBufferTime;
	MaxServerInputsPerSecond = 30;
	WeaponTraceStartSocket = TEXT("weapon_base");
	WeaponTraceEndSocket = TEXT("weapon_tip");
	HitTraceChannel = ECC_GameTraceChannel1; // EMBMeleeHit (Config/DefaultEngine.ini)
	MaxHitSubSteps = 6;
	SubStepTravelFactor = 1.5f;
	bAllowFriendlyFire = false;

	// Combo léger : 3 coups, le 3e frappe plus fort (x1,2).
	LightComboAttacks.SetNum(EMBCombatDefaults::LightComboLength);
	for (int32 Index = 0; Index < LightComboAttacks.Num(); ++Index)
	{
		FEMBAttackDefinition& Def = LightComboAttacks[Index];
		Def.StaminaCost = EMBCombatDefaults::LightStaminaCost;
		Def.DamageMultiplier = (Index == EMBCombatDefaults::LightComboLength - 1)
			? EMBCombatDefaults::LightThirdHitDamageMultiplier
			: EMBCombatDefaults::LightDamageMultiplier;
		Def.PoiseDamage = EMBCombatDefaults::LightPoiseDamage;
	}

	HeavyAttack.StaminaCost = EMBCombatDefaults::HeavyStaminaCost;
	HeavyAttack.DamageMultiplier = EMBCombatDefaults::HeavyDamageMultiplier;
	HeavyAttack.PoiseDamage = EMBCombatDefaults::HeavyPoiseDamage;

	ChargedAttack.StaminaCost = EMBCombatDefaults::ChargedStaminaCost;
	ChargedAttack.DamageMultiplier = EMBCombatDefaults::ChargedDamageMultiplierMin;
	ChargedAttack.PoiseDamage = EMBCombatDefaults::ChargedPoiseDamageMin;

	CurrentState = EEMBCombatState::Idle;
	ComboIndex = 0;
	LastAttackState = EEMBCombatState::Idle;

	bComboWindowOpen = false;
	bDodgeCancelOpen = false;
	bParryWindowOpen = false;
	bParryWhiffed = false;
	bParryLanded = false;
	bCharging = false;
	bHitWindowStarted = false;
	bHitWindowActive = false;
	InvulnerableCount = 0;
	HyperArmorCount = 0;
	bBlockHeld = false;
	bChargeHeld = false;

	ChargeStartTime = 0.0;
	BlockGuardUpTime = 0.0;
	LastParryTime = -1.0;
	BufferedInputTime = 0.0;
	InputRateWindowStart = 0.0;
	InputRateCount = 0;
	BufferedInput = EEMBCombatInput::None;

	EncumbranceRatio = 0.f;
	PendingStaggerDuration = 0.f;
	bPendingGuardBreak = false;
	ActionToken = 0;
	bServerMontageStarted = false;

	PrevWeaponBase = FVector::ZeroVector;
	PrevWeaponTip = FVector::ZeroVector;
	bHasPrevWeaponPose = false;
}

void UEMBCombatComponent::BeginPlay()
{
	Super::BeginPlay();

	AActor* Owner = GetOwner();
	Stats = Owner ? Owner->FindComponentByClass<UEMBStatsComponent>() : nullptr;
	if (!Stats)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBCombatComponent sur %s : aucun UEMBStatsComponent trouvé, le combat est inopérant."), *GetNameSafe(Owner));
	}

	// Les tags natifs ne sont pas résolus dans le constructeur (voir EMBGameplayTags.h) : on complète ici.
	ResolveDefaultTags();

	// La logique de jeu (mort, rupture de poise) ne tourne que sur le serveur.
	if (Stats && HasAuthorityOwner())
	{
		Stats->OnDeath.AddUniqueDynamic(this, &UEMBCombatComponent::HandleOwnerDeath);
		Stats->OnPoiseBroken.AddUniqueDynamic(this, &UEMBCombatComponent::HandleOwnerPoiseBroken);
	}
}

void UEMBCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearActionTimers();
	if (const UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BufferExpiryTimer);
		World->GetTimerManager().ClearTimer(SpawnProtectionTimer);
	}

	if (Stats)
	{
		Stats->OnDeath.RemoveDynamic(this, &UEMBCombatComponent::HandleOwnerDeath);
		Stats->OnPoiseBroken.RemoveDynamic(this, &UEMBCombatComponent::HandleOwnerPoiseBroken);
	}

	Super::EndPlay(EndPlayReason);
}

void UEMBCombatComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UEMBCombatComponent, CurrentState);
	DOREPLIFETIME(UEMBCombatComponent, ComboIndex);
	DOREPLIFETIME(UEMBCombatComponent, bSpawnProtected); // Visible des clients : halo d'invulnérabilité de réapparition.
}

void UEMBCombatComponent::ResolveDefaultTags()
{
	for (FEMBAttackDefinition& Def : LightComboAttacks)
	{
		if (!Def.AttackTag.IsValid())
		{
			Def.AttackTag = EMBTags::Attack_Player_Light.GetTag();
		}
	}
	if (!HeavyAttack.AttackTag.IsValid())
	{
		HeavyAttack.AttackTag = EMBTags::Attack_Player_Heavy.GetTag();
	}
	if (!ChargedAttack.AttackTag.IsValid())
	{
		ChargedAttack.AttackTag = EMBTags::Attack_Player_Charged.GetTag();
	}
}

bool UEMBCombatComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

bool UEMBCombatComponent::IsAttackState(EEMBCombatState State)
{
	return State == EEMBCombatState::LightAttack
		|| State == EEMBCombatState::HeavyAttack
		|| State == EEMBCombatState::ChargedAttack;
}

// ================================================================================================
// Configuration à l'exécution
// ================================================================================================

void UEMBCombatComponent::SetWeaponMesh(UMeshComponent* NewWeaponMesh)
{
	WeaponMesh = NewWeaponMesh;
	bHasPrevWeaponPose = false;
}

void UEMBCombatComponent::SetWeaponBaseDamage(float NewDamage)
{
	WeaponBaseDamage = FMath::Max(0.f, NewDamage);
}

void UEMBCombatComponent::SetEncumbranceRatio(float NewRatio)
{
	EncumbranceRatio = FMath::Max(0.f, NewRatio);
}

// ================================================================================================
// Tags d'état
// ================================================================================================

bool UEMBCombatComponent::IsInvulnerable() const
{
	return InvulnerableCount > 0 || bSpawnProtected;
}

float UEMBCombatComponent::GetFireResistance() const
{
	// Passif "Communion draconique" + bonus temporaires (huile), plafonnés à 0,9.
	return FMath::Clamp(PassiveModifiers.FireResistance + ExtraFireResistance, 0.f, EMBCombatDefaults::MaxFireResistance);
}

void UEMBCombatComponent::SetExtraFireResistance(float Bonus)
{
	ExtraFireResistance = FMath::IsFinite(Bonus) ? FMath::Max(0.f, Bonus) : 0.f;
}

void UEMBCombatComponent::GrantSpawnProtection(float Seconds)
{
	if (!HasAuthorityOwner() || !GetWorld() || Seconds <= 0.f)
	{
		return;
	}

	// Minuteur dédié (et non ActionTimers, vidés à chaque nouvelle action) : l'invulnérabilité survit aux
	// changements d'état, mais pas à la première attaque (CancelSpawnProtection dans BeginAttackAction).
	bSpawnProtected = true;
	GetWorld()->GetTimerManager().SetTimer(
		SpawnProtectionTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]() { bSpawnProtected = false; }),
		Seconds, false);
}

void UEMBCombatComponent::CancelSpawnProtection()
{
	if (bSpawnProtected)
	{
		bSpawnProtected = false;
		if (const UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(SpawnProtectionTimer);
		}
	}
}

void UEMBCombatComponent::ResetFromDeath()
{
	if (!HasAuthorityOwner() || !GetWorld() || CurrentState != EEMBCombatState::Dead)
	{
		return;
	}

	// Dead est un état sans sortie dans la table de transitions (CanTransitionTo) : la réanimation passe
	// donc par une sortie explicite, réservée à cette fonction.
	const EEMBCombatState OldState = CurrentState;
	ExitState(OldState, EEMBCombatState::Idle);
	CurrentState = EEMBCombatState::Idle;
	BufferedInput = EEMBCombatInput::None;
	RiposteTarget.Reset();
	bKilledInLethalZone = false;
	EnterState(EEMBCombatState::Idle, OldState);
	OnCombatStateChanged.Broadcast(OldState, EEMBCombatState::Idle);
}

bool UEMBCombatComponent::HasStateTag(FGameplayTag Tag) const
{
	if (!Tag.IsValid())
	{
		return false;
	}
	if (Tag.MatchesTagExact(EMBTags::State_Combat_Invulnerable.GetTag()))
	{
		return IsInvulnerable();
	}
	if (Tag.MatchesTagExact(EMBTags::State_Combat_HyperArmor.GetTag()))
	{
		return HyperArmorCount > 0;
	}
	if (Tag.MatchesTagExact(EMBTags::State_Combat_Staggered.GetTag()))
	{
		return CurrentState == EEMBCombatState::Staggered;
	}
	if (Tag.MatchesTagExact(EMBTags::State_Dead.GetTag()))
	{
		return CurrentState == EEMBCombatState::Dead;
	}
	return false;
}

void UEMBCombatComponent::BeginStateTag(const FGameplayTag& Tag)
{
	// Tag vide ou inconnu : le notify IFrames sert par défaut à l'invulnérabilité.
	if (Tag.IsValid() && Tag.MatchesTagExact(EMBTags::State_Combat_HyperArmor.GetTag()))
	{
		++HyperArmorCount;
	}
	else
	{
		++InvulnerableCount;
	}
}

void UEMBCombatComponent::EndStateTag(const FGameplayTag& Tag)
{
	// Clamp à 0 : une fin de notify tardive après une remise à zéro (changement d'état) ne doit rien casser.
	if (Tag.IsValid() && Tag.MatchesTagExact(EMBTags::State_Combat_HyperArmor.GetTag()))
	{
		HyperArmorCount = FMath::Max(0, HyperArmorCount - 1);
	}
	else
	{
		InvulnerableCount = FMath::Max(0, InvulnerableCount - 1);
	}
}

bool UEMBCombatComponent::IsRiposteWindowOpen() const
{
	const UWorld* World = GetWorld();
	return World && LastParryTime >= 0.0
		&& (World->GetTimeSeconds() - LastParryTime) <= EMBCombatDefaults::ParryRiposteWindow;
}

void UEMBCombatComponent::ResetTransientFlags(EEMBCombatState NewState)
{
	InvulnerableCount = 0;
	HyperArmorCount = 0;
	bDodgeCancelOpen = false;
	bParryWindowOpen = false;
	bParryWhiffed = false;
	bParryLanded = false;
	bHitWindowActive = false;

	// La fenêtre de combo s'ouvre à l'instant même où l'attaque entre en Recovery (frame 18) :
	// elle doit survivre à ce passage, et seulement à celui-ci.
	if (NewState != EEMBCombatState::Recovery)
	{
		bComboWindowOpen = false;
	}
}

// ================================================================================================
// Table de transitions
// ================================================================================================

bool UEMBCombatComponent::CanTransitionTo(EEMBCombatState NewState) const
{
	const EEMBCombatState From = CurrentState;

	// Règles transversales.
	if (From == EEMBCombatState::Dead)
	{
		return false; // Aucune sortie de la mort.
	}
	if (NewState == EEMBCombatState::Dead)
	{
		return true; // On peut mourir de partout.
	}
	if (NewState == EEMBCombatState::Staggered)
	{
		return From != EEMBCombatState::Staggered; // Un étourdi n'est pas ré-étourdi (pas de stun-lock).
	}

	// Une action de départ : tout ce qu'on peut lancer depuis un état "libre".
	const bool bIsActionStart = NewState == EEMBCombatState::LightAttack
		|| NewState == EEMBCombatState::HeavyAttack
		|| NewState == EEMBCombatState::ChargedAttack
		|| NewState == EEMBCombatState::Dodge
		|| NewState == EEMBCombatState::Parry;

	switch (From)
	{
	case EEMBCombatState::Idle:
		return bIsActionStart || NewState == EEMBCombatState::Block;

	case EEMBCombatState::Block:
		// Depuis la garde : relâcher, ou enchaîner directement une action (roulade, parade, attaque).
		return NewState == EEMBCombatState::Idle || bIsActionStart;

	case EEMBCombatState::LightAttack:
	case EEMBCombatState::HeavyAttack:
		// Fin de la fenêtre active -> Recovery ; fin du montage -> Idle.
		return NewState == EEMBCombatState::Recovery || NewState == EEMBCombatState::Idle;

	case EEMBCombatState::ChargedAttack:
		return NewState == EEMBCombatState::Recovery
			|| NewState == EEMBCombatState::Idle
			// Relâché avant 0,4 s : la charge devient une attaque lourde.
			|| (NewState == EEMBCombatState::HeavyAttack && bCharging)
			// Pendant la charge, seule la roulade annule (l'endurance de la charge n'a pas encore été dépensée).
			|| (NewState == EEMBCombatState::Dodge && bCharging);

	case EEMBCombatState::Dodge:
		if (NewState == EEMBCombatState::Idle)
		{
			return true;
		}
		// Annulation de la roulade à partir de 0,50 s (frame 30).
		return bDodgeCancelOpen && (bIsActionStart || NewState == EEMBCombatState::Block);

	case EEMBCombatState::Parry:
		return NewState == EEMBCombatState::Idle;

	case EEMBCombatState::Staggered:
		return NewState == EEMBCombatState::Idle;

	case EEMBCombatState::Recovery:
		if (NewState == EEMBCombatState::Idle)
		{
			return true;
		}
		// Fenêtre de combo : seule une attaque légère l'ouvre, et seules des attaques peuvent la consommer.
		if (NewState == EEMBCombatState::LightAttack || NewState == EEMBCombatState::HeavyAttack)
		{
			return bComboWindowOpen && LastAttackState == EEMBCombatState::LightAttack;
		}
		return false;

	default:
		return false;
	}
}

bool UEMBCombatComponent::TransitionTo(EEMBCombatState NewState)
{
	if (!HasAuthorityOwner() || !GetWorld() || !CanTransitionTo(NewState))
	{
		return false;
	}

	const EEMBCombatState OldState = CurrentState;
	ExitState(OldState, NewState);
	CurrentState = NewState; // Répliqué : les clients déclenchent OnRep_CurrentState.
	EnterState(NewState, OldState);
	OnCombatStateChanged.Broadcast(OldState, NewState);

	// Moments où une transition devient possible : on consomme l'input mémorisé.
	// (Fait en dernier : la consommation peut elle-même enchaîner une autre transition.)
	if (NewState == EEMBCombatState::Idle)
	{
		TryConsumeBufferedInput();
		// Garde toujours maintenue : on la reprend d'elle-même une fois libre.
		if (CurrentState == EEMBCombatState::Idle && bBlockHeld)
		{
			TransitionTo(EEMBCombatState::Block);
		}
	}
	else if (NewState == EEMBCombatState::Recovery && bComboWindowOpen)
	{
		TryConsumeBufferedInput();
	}
	return true;
}

void UEMBCombatComponent::ExitState(EEMBCombatState OldState, EEMBCombatState NewState)
{
	switch (OldState)
	{
	case EEMBCombatState::Block:
		if (Stats)
		{
			Stats->SetStaminaRegenMultiplier(1.f);
		}
		break;
	case EEMBCombatState::ChargedAttack:
		bCharging = false;
		break;
	default:
		break;
	}
}

void UEMBCombatComponent::EnterState(EEMBCombatState NewState, EEMBCombatState OldState)
{
	const double Now = GetWorld()->GetTimeSeconds();

	// Une nouvelle action invalide les temporisateurs et callbacks de la précédente. La Recovery n'est
	// pas une nouvelle action : elle prolonge l'attaque (montage, fenêtre de combo, temporisateurs).
	if (NewState != EEMBCombatState::Recovery)
	{
		++ActionToken;
		ClearActionTimers();
	}
	ResetTransientFlags(NewState);

	switch (NewState)
	{
	case EEMBCombatState::Idle:
		ComboIndex = 0;
		ActiveMontage.Reset();
		// La garde et la charge sont des montages en boucle : il faut les arrêter explicitement.
		// Arrêt ciblé sur le montage concerné (jamais global) : une action lancée juste après, dont le
		// multicast pourrait arriver avant cet arrêt chez un client, ne peut ainsi pas être coupée.
		if (OldState == EEMBCombatState::Block && BlockMontage)
		{
			Multicast_StopMontage(BlockMontage, 0.15f);
		}
		else if (OldState == EEMBCombatState::ChargedAttack && ChargeLoopMontage)
		{
			Multicast_StopMontage(ChargeLoopMontage, 0.15f);
		}
		else if (OldState == EEMBCombatState::Dead && DeathMontage)
		{
			Multicast_StopMontage(DeathMontage, 0.2f); // Réanimé : on relève le personnage.
		}
		break;

	case EEMBCombatState::LightAttack:
	{
		const int32 Index = FMath::Clamp<int32>(ComboIndex, 0, LightComboAttacks.Num() - 1);
		BeginAttackAction(LightComboAttacks[Index], NewState);
		break;
	}

	case EEMBCombatState::HeavyAttack:
		BeginAttackAction(HeavyAttack, NewState);
		// Hyper-armure frames 6-31 : pilotée par le code (constantes du référentiel), pas par l'animation.
		ScheduleStateTagWindow(EMBCombatDefaults::HeavyHyperArmorStart, EMBCombatDefaults::HeavyHyperArmorEnd, true);
		break;

	case EEMBCombatState::ChargedAttack:
		// Phase de charge : hyper-armure dès le début, jusqu'à la fin de la fenêtre active (voir EndHitWindow).
		CancelSpawnProtection(); // Charger une attaque compte comme attaquer.
		bCharging = true;
		ChargeStartTime = Now;
		LastAttackState = NewState;
		bHitWindowStarted = false;
		HyperArmorCount = 1;
		PlayActionMontage(ChargeLoopMontage, 1.f, false, /*bReliable=*/true);
		// Relâchement automatique au plafond de charge (1,2 s), même si le bouton reste enfoncé.
		ScheduleActionTimer(EMBCombatDefaults::ChargedMaxChargeTime, [this]()
		{
			if (CurrentState == EEMBCombatState::ChargedAttack && bCharging)
			{
				ReleaseCharge();
			}
		});
		break;

	case EEMBCombatState::Dodge:
	{
		/*
		 * I-frames pilotées par timer selon la charge portée (13 / 11 / 9 frames) : valeur de conception
		 * du référentiel, indépendante du montage et de l'évaluation d'animation du serveur. Le notify
		 * IFrames des montages reste possible mais optionnel : les compteurs sont cumulatifs, l'invulnérabilité
		 * dure tant que l'un d'eux est actif (un notify plus long que la règle l'allongerait donc).
		 */
		ScheduleStateTagWindow(EMBCombatDefaults::DodgeIFrameStart, GetDodgeIFrameEnd(), false);
		if (!PlayActionMontage(GetDodgeMontage(), 1.f, true))
		{
			ScheduleFallbackTimeline(NewState);
		}
		ScheduleActionTimer(EMBCombatDefaults::DodgeCancelTime, [this]()
		{
			bDodgeCancelOpen = true;
			TryConsumeBufferedInput();
		});
		break;
	}

	case EEMBCombatState::Parry:
	{
		/*
		 * La fenêtre de parade parfaite (frames 4-13, soit 10 frames = 0,167 s) est pilotée par un
		 * temporisateur serveur et non par un AnimNotifyState, pour trois raisons : c'est une constante de
		 * gameplay du référentiel (elle ne doit pas dépendre d'un montage retouché par un animateur) ;
		 * elle reste exacte même si le mesh n'est pas animé sur le serveur dédié ; et elle évite toute
		 * désynchronisation entre le montage et la règle. ParryLatencyTolerance permet de la compenser.
		 */
		ScheduleActionTimer(EMBCombatDefaults::ParryPerfectStart, [this]() { bParryWindowOpen = true; });
		ScheduleActionTimer(EMBCombatDefaults::ParryPerfectEnd + ParryLatencyTolerance
			+ PassiveModifiers.ParryWindowFrames * EMBCombatDefaults::FrameTime, [this]() // passif "Parade du maître" : +2 frames par rang
		{
			// Parade ratée : vulnérable (counter-hit x1,25) jusqu'à la fin du montage (frame 44).
			// Sauf si une parade parfaite a déjà réussi dans cette fenêtre.
			bParryWindowOpen = false;
			bParryWhiffed = !bParryLanded;
		});
		if (!PlayActionMontage(ParryMontage, 1.f, true))
		{
			ScheduleFallbackTimeline(NewState);
		}
		break;
	}

	case EEMBCombatState::Block:
		BlockGuardUpTime = Now + EMBCombatDefaults::BlockRaiseTime; // La garde n'est efficace qu'après 0,10 s.
		if (Stats)
		{
			Stats->SetStaminaRegenMultiplier(EMBCombatDefaults::BlockStaminaRegenMultiplier);
		}
		PlayActionMontage(BlockMontage, 1.f, false, /*bReliable=*/true);
		break;

	case EEMBCombatState::Staggered:
	{
		bCharging = false;
		BufferedInput = EEMBCombatInput::None; // Un coup reçu annule l'intention en cours.

		UAnimMontage* StaggerAnim = bPendingGuardBreak ? GuardBreakMontage.Get() : StaggerMontage.Get();
		float PlayRate = 1.f;
		if (StaggerAnim && PendingStaggerDuration > KINDA_SMALL_NUMBER)
		{
			// Le montage épouse exactement la durée d'étourdissement du référentiel (0,8 s ou 1,2 s).
			PlayRate = FMath::Clamp(StaggerAnim->GetPlayLength() / PendingStaggerDuration, MinStaggerPlayRate, MaxStaggerPlayRate);
		}
		PlayActionMontage(StaggerAnim, PlayRate, false, /*bReliable=*/true);

		// La durée est pilotée par un temporisateur (et non par la fin du montage) : c'est la valeur de conception.
		ScheduleActionTimer(FMath::Max(PendingStaggerDuration, KINDA_SMALL_NUMBER), [this]()
		{
			TransitionTo(EEMBCombatState::Idle);
		});
		break;
	}

	case EEMBCombatState::Recovery:
		// Rien à lancer : le montage de l'attaque se poursuit jusqu'à son terme (OnActionFinished -> Idle).
		break;

	case EEMBCombatState::Dead:
		CancelSpawnProtection();
		bCharging = false;
		bBlockHeld = false;
		bChargeHeld = false;
		BufferedInput = EEMBCombatInput::None;
		if (Stats)
		{
			Stats->SetStaminaRegenMultiplier(1.f);
		}
		PlayActionMontage(DeathMontage, 1.f, false, /*bReliable=*/true);
		break;
	}
}

void UEMBCombatComponent::EnterStaggered(float Duration, bool bGuardBreak)
{
	PendingStaggerDuration = Duration;
	bPendingGuardBreak = bGuardBreak;
	TransitionTo(EEMBCombatState::Staggered);
}

void UEMBCombatComponent::OnActionFinished()
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	switch (CurrentState)
	{
	case EEMBCombatState::LightAttack:
	case EEMBCombatState::HeavyAttack:
	case EEMBCombatState::Dodge:
	case EEMBCombatState::Parry:
	case EEMBCombatState::Recovery:
		TransitionTo(EEMBCombatState::Idle);
		break;

	case EEMBCombatState::ChargedAttack:
		if (!bCharging) // Pendant la charge, le montage en boucle n'a pas de fin.
		{
			TransitionTo(EEMBCombatState::Idle);
		}
		break;

	default:
		// Block (maintenue), Staggered (temporisateur), Dead : pas de fin pilotée par le montage.
		break;
	}
}

void UEMBCombatComponent::HandleOwnerDeath(AActor* Killer)
{
	bKilledInLethalZone = bProcessingLethalZone; // Lu par le personnage dans BeginDeath.
	TransitionTo(EEMBCombatState::Dead);
}

void UEMBCombatComponent::HandleOwnerPoiseBroken(AActor* InInstigator)
{
	// Poise brisée : étourdissement de 0,8 s (48 f). Sans effet si déjà étourdi ou mort (CanTransitionTo).
	EnterStaggered(EMBCombatDefaults::PoiseBreakStaggerDuration, false);
}

void UEMBCombatComponent::OnRep_CurrentState(EEMBCombatState OldState)
{
	// Côté client uniquement : le serveur diffuse directement dans TransitionTo.
	OnCombatStateChanged.Broadcast(OldState, CurrentState);
}

// ================================================================================================
// Entrées : client -> serveur
// ================================================================================================

void UEMBCombatComponent::HandleInput(EEMBCombatInput Input)
{
	AActor* Owner = GetOwner();
	if (!Owner || Input == EEMBCombatInput::None)
	{
		return;
	}

	// Serveur (dédié, hôte d'une partie listen ou solo) : exécution directe.
	if (Owner->HasAuthority())
	{
		ProcessInput(Input);
		return;
	}

	// Client : seul le pawn contrôlé localement envoie des inputs.
	const APawn* OwnerPawn = Cast<APawn>(Owner);
	if (!OwnerPawn || !OwnerPawn->IsLocallyControlled() || CurrentState == EEMBCombatState::Dead)
	{
		return;
	}

	PredictLocally(Input);
	Server_HandleInput(Input); // Reliable : une action discrète perdue serait un bug visible.
}

bool UEMBCombatComponent::Server_HandleInput_Validate(EEMBCombatInput Input)
{
	// Un client modifié ne doit pas pouvoir injecter une valeur d'énumération hors plage.
	return static_cast<uint8>(Input) <= static_cast<uint8>(EEMBCombatInput::BlockEnd);
}

void UEMBCombatComponent::Server_HandleInput_Implementation(EEMBCombatInput Input)
{
	ProcessInput(Input);
}

bool UEMBCombatComponent::AllowServerInput()
{
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - InputRateWindowStart >= 1.0)
	{
		InputRateWindowStart = Now;
		InputRateCount = 0;
	}
	return ++InputRateCount <= MaxServerInputsPerSecond;
}

bool UEMBCombatComponent::IsBufferable(EEMBCombatInput Input) const
{
	// BlockStart/BlockEnd et ChargedAttackRelease sont des états maintenus (drapeaux), pas des événements à mémoriser.
	return Input == EEMBCombatInput::LightAttack
		|| Input == EEMBCombatInput::HeavyAttack
		|| Input == EEMBCombatInput::ChargedAttackStart
		|| Input == EEMBCombatInput::Dodge
		|| Input == EEMBCombatInput::Parry;
}

void UEMBCombatComponent::BufferInput(EEMBCombatInput Input)
{
	BufferedInput = Input; // Un seul emplacement : le dernier input gagne.
	BufferedInputTime = GetWorld()->GetTimeSeconds();
	ArmBufferExpiry();
}

void UEMBCombatComponent::ArmBufferExpiry()
{
	// Si l'input mémorisé n'est jamais consommé, on le jette à l'expiration et on dit au propriétaire
	// d'annuler le montage qu'il avait prédit (sinon son animation resterait sans suite côté serveur).
	FTimerManager& TimerManager = GetWorld()->GetTimerManager();
	TimerManager.SetTimer(
		BufferExpiryTimer,
		FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (BufferedInput != EEMBCombatInput::None)
			{
				BufferedInput = EEMBCombatInput::None;
				Client_RejectPrediction();
			}
		}),
		FMath::Max(InputBufferTime, KINDA_SMALL_NUMBER) + 0.02f, // petite marge : l'expiration "naturelle" passe en premier
		false);
}

void UEMBCombatComponent::TryConsumeBufferedInput()
{
	if (!HasAuthorityOwner() || BufferedInput == EEMBCombatInput::None || !GetWorld())
	{
		return;
	}

	if (GetWorld()->GetTimeSeconds() - BufferedInputTime > InputBufferTime)
	{
		BufferedInput = EEMBCombatInput::None; // Expiré (0,2 s).
		Client_RejectPrediction();
		return;
	}

	const EEMBCombatInput Original = BufferedInput;
	EEMBCombatInput ToExecute = Original;

	// Charge pressée puis relâchée alors qu'on ne pouvait pas encore agir : c'était un simple appui = attaque lourde.
	if (ToExecute == EEMBCombatInput::ChargedAttackStart && !bChargeHeld)
	{
		ToExecute = EEMBCombatInput::HeavyAttack;
	}

	BufferedInput = EEMBCombatInput::None;
	if (!ExecuteInput(ToExecute))
	{
		// Toujours impossible : on le garde jusqu'à l'expiration ou la prochaine transition possible.
		BufferedInput = Original;
	}
}

void UEMBCombatComponent::ProcessInput(EEMBCombatInput Input)
{
	if (!HasAuthorityOwner() || Input == EEMBCombatInput::None || !GetWorld())
	{
		return;
	}
	if (CurrentState == EEMBCombatState::Dead)
	{
		return;
	}

	// Les relâchements (fin de garde, fin de charge) ne sont jamais soumis au plafond d'inputs : les perdre
	// laisserait une garde ou une charge bloquée. Seules les actions "pressées" sont limitées.
	const bool bIsRelease = Input == EEMBCombatInput::BlockEnd || Input == EEMBCombatInput::ChargedAttackRelease;
	if (!bIsRelease && !AllowServerInput())
	{
		return;
	}

	switch (Input)
	{
	case EEMBCombatInput::ChargedAttackRelease:
		bChargeHeld = false;
		if (CurrentState == EEMBCombatState::ChargedAttack && bCharging)
		{
			ReleaseCharge();
		}
		return;

	case EEMBCombatInput::BlockEnd:
		bBlockHeld = false;
		if (CurrentState == EEMBCombatState::Block)
		{
			TransitionTo(EEMBCombatState::Idle);
		}
		return;

	case EEMBCombatInput::BlockStart:
		bBlockHeld = true;
		// Si une action est en cours, le drapeau fera lever la garde dès le retour à Idle.
		ExecuteInput(Input);
		return;

	case EEMBCombatInput::ChargedAttackStart:
		bChargeHeld = true;
		break;

	default:
		break;
	}

	if (ExecuteInput(Input))
	{
		return;
	}

	// Impossible maintenant : on mémorise (0,2 s) pour le consommer à la prochaine transition possible
	// (fin de recovery, ouverture de la fenêtre de combo, fin de roulade annulable).
	if (IsBufferable(Input) && CurrentState != EEMBCombatState::Staggered)
	{
		BufferInput(Input);
		return;
	}

	// Rien n'a été fait : on prévient le propriétaire d'annuler son montage prédit.
	Client_RejectPrediction();
}

bool UEMBCombatComponent::ExecuteInput(EEMBCombatInput Input)
{
	switch (Input)
	{
	case EEMBCombatInput::LightAttack:         return TryStartLightAttack();
	case EEMBCombatInput::HeavyAttack:         return TryStartHeavyAttack();
	case EEMBCombatInput::ChargedAttackStart:  return TryStartCharge();
	case EEMBCombatInput::Dodge:               return TryStartDodge();
	case EEMBCombatInput::Parry:               return TryStartParry();
	case EEMBCombatInput::BlockStart:          return TryStartBlock();
	default:                                   return false;
	}
}

bool UEMBCombatComponent::TryStartLightAttack()
{
	if (!Stats || LightComboAttacks.Num() == 0 || !CanTransitionTo(EEMBCombatState::LightAttack))
	{
		return false;
	}

	// Depuis la Recovery (fenêtre de combo) on enchaîne le coup suivant ; sinon on repart du premier.
	const bool bChaining = CurrentState == EEMBCombatState::Recovery;
	const int32 NextIndex = bChaining ? static_cast<int32>(ComboIndex) + 1 : 0;
	if (NextIndex >= LightComboAttacks.Num())
	{
		return false; // Combo terminé : il faut repasser par Idle.
	}

	if (!Stats->ConsumeStamina(ScaledAttackCost(LightComboAttacks[NextIndex].StaminaCost)))
	{
		return false;
	}

	ComboIndex = static_cast<uint8>(NextIndex);
	return TransitionTo(EEMBCombatState::LightAttack);
}

bool UEMBCombatComponent::TryStartHeavyAttack()
{
	if (!Stats || !CanTransitionTo(EEMBCombatState::HeavyAttack) || !Stats->ConsumeStamina(ScaledAttackCost(HeavyAttack.StaminaCost)))
	{
		return false;
	}
	return TransitionTo(EEMBCombatState::HeavyAttack);
}

bool UEMBCombatComponent::TryStartCharge()
{
	// L'endurance (38) n'est consommée qu'au relâchement ; au départ, il suffit que la jauge ne soit pas vide.
	if (!Stats || !Stats->CanAffordStamina() || !CanTransitionTo(EEMBCombatState::ChargedAttack))
	{
		return false;
	}
	return TransitionTo(EEMBCombatState::ChargedAttack);
}

bool UEMBCombatComponent::TryStartDodge()
{
	// Surchargé (> 100 % de la charge) : pas de roulade.
	if (!Stats || EncumbranceRatio > EMBCombatDefaults::LoadHeavyMax || !CanTransitionTo(EEMBCombatState::Dodge))
	{
		return false;
	}

	const float Cost = (EncumbranceRatio >= EMBCombatDefaults::LoadMediumMax)
		? EMBCombatDefaults::DodgeHeavyLoadStaminaCost
		: EMBCombatDefaults::DodgeStaminaCost;
	if (!Stats->ConsumeStamina(Cost))
	{
		return false;
	}
	return TransitionTo(EEMBCombatState::Dodge);
}

bool UEMBCombatComponent::TryStartParry()
{
	if (!Stats || !CanTransitionTo(EEMBCombatState::Parry) || !Stats->ConsumeStamina(EMBCombatDefaults::ParryStaminaCost))
	{
		return false;
	}
	return TransitionTo(EEMBCombatState::Parry);
}

bool UEMBCombatComponent::TryStartBlock()
{
	if (!CanTransitionTo(EEMBCombatState::Block))
	{
		return false;
	}
	return TransitionTo(EEMBCombatState::Block);
}

void UEMBCombatComponent::ReleaseCharge()
{
	if (!Stats)
	{
		TransitionTo(EEMBCombatState::Idle);
		return;
	}

	const double Held = GetWorld()->GetTimeSeconds() - ChargeStartTime;

	// Relâché avant 0,4 s : simple appui = attaque lourde (coût 26).
	if (Held < EMBCombatDefaults::ChargedMinChargeTime)
	{
		if (Stats->ConsumeStamina(ScaledAttackCost(HeavyAttack.StaminaCost)) && TransitionTo(EEMBCombatState::HeavyAttack))
		{
			return;
		}
		TransitionTo(EEMBCombatState::Idle);
		return;
	}

	// Frappe chargée : coût de 38 au relâchement.
	if (!Stats->ConsumeStamina(ScaledAttackCost(ChargedAttack.StaminaCost)))
	{
		TransitionTo(EEMBCombatState::Idle);
		return;
	}

	// Interpolation linéaire entre 0,4 s et 1,2 s : multiplicateur 1,6 -> 2,4 et poise 35 -> 55.
	const float Span = EMBCombatDefaults::ChargedMaxChargeTime - EMBCombatDefaults::ChargedMinChargeTime;
	const float Alpha = FMath::Clamp((static_cast<float>(Held) - EMBCombatDefaults::ChargedMinChargeTime) / Span, 0.f, 1.f);

	FEMBAttackDefinition Strike = ChargedAttack;
	Strike.DamageMultiplier = FMath::Lerp(ChargedAttack.DamageMultiplier, EMBCombatDefaults::ChargedDamageMultiplierMax, Alpha);
	Strike.PoiseDamage = FMath::Lerp(ChargedAttack.PoiseDamage, EMBCombatDefaults::ChargedPoiseDamageMax, Alpha);

	// La charge reste dans l'état ChargedAttack (l'hyper-armure continue) : seule la phase change.
	// On invalide cependant les temporisateurs / callbacks de la phase de charge.
	bCharging = false;
	++ActionToken;
	ClearActionTimers();

	ActiveAttack = Strike;
	bHitWindowStarted = false;
	bHitWindowActive = false;
	if (!PlayActionMontage(Strike.Montage, Strike.PlayRate, true))
	{
		ScheduleFallbackTimeline(EEMBCombatState::ChargedAttack);
	}
}

void UEMBCombatComponent::BeginAttackAction(const FEMBAttackDefinition& Definition, EEMBCombatState State)
{
	CancelSpawnProtection(); // Attaquer met fin à l'invulnérabilité de réapparition.
	ActiveAttack = Definition;
	LastAttackState = State;
	bHitWindowStarted = false;
	bHitWindowActive = false;

	if (!PlayActionMontage(Definition.Montage, Definition.PlayRate, true))
	{
		ScheduleFallbackTimeline(State);
	}
}

// ================================================================================================
// Notifies (fenêtres d'animation)
// ================================================================================================

UEMBCombatComponent* UEMBCombatComponent::FindFromMesh(const USkeletalMeshComponent* MeshComp)
{
	// Un notify est un asset partagé entre toutes les instances : il ne peut pas mémoriser le composant.
	// FindComponentByClass ne parcourt que la poignée de composants du personnage : coût négligeable.
	const AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	return Owner ? Owner->FindComponentByClass<UEMBCombatComponent>() : nullptr;
}

void UEMBCombatComponent::SetComboWindowOpen(bool bOpen)
{
	bComboWindowOpen = bOpen;
	if (bOpen && HasAuthorityOwner())
	{
		TryConsumeBufferedInput();
	}
}

void UEMBCombatComponent::BeginHitWindow(const FEMBHitWindowParams& Params)
{
	if (!HasAuthorityOwner() || !IsAttackState(CurrentState))
	{
		return;
	}
	if (CurrentState == EEMBCombatState::ChargedAttack && bCharging)
	{
		return; // Pas de coup pendant la charge.
	}

	ActiveHitParams = Params;
	bHitWindowActive = true;
	bHitWindowStarted = true; // L'attaque quitte son "startup" : fin de la zone de counter-hit.
	HitActorsThisWindow.Reset();

	FVector Base;
	FVector Tip;
	bHasPrevWeaponPose = GetWeaponSocketLocations(Base, Tip);
	if (bHasPrevWeaponPose)
	{
		PrevWeaponBase = Base;
		PrevWeaponTip = Tip;
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("HitWindow sur %s : sockets '%s' / '%s' introuvables, aucun coup ne sera détecté."),
			*GetNameSafe(GetOwner()), *WeaponTraceStartSocket.ToString(), *WeaponTraceEndSocket.ToString());
	}
}

void UEMBCombatComponent::TickHitWindow(float DeltaTime)
{
	if (!HasAuthorityOwner() || !bHitWindowActive)
	{
		return;
	}

	/*
	 * COÛT : serveur uniquement, et seulement pendant la fenêtre active (6 à 8 frames par coup).
	 * Par tick d'animation : 2 lectures de socket + de 1 à MaxHitSubSteps balayages sphériques
	 * (SweepMultiByChannel) le long de la lame, sur un canal dédié qui ne voit que pawns et décor.
	 * Les sous-pas ne sont ajoutés que si l'arme s'est déplacée de plus de (rayon x SubStepTravelFactor)
	 * depuis le dernier tick : à 60 fps un seul balayage suffit ; à 15 fps le coup est découpé en
	 * 4 à 6 positions interpolées entre la pose précédente et la pose actuelle, ce qui évite qu'un coup
	 * rapide "traverse" une cible. Le résultat est borné par MaxHitSubSteps, donc le pire cas est constant.
	 * Les poses des sockets sont celles de la dernière évaluation d'animation (décalage d'une frame au
	 * plus) : l'interpolation absorbe ce décalage.
	 */
	FVector CurrentBase;
	FVector CurrentTip;
	if (!GetWeaponSocketLocations(CurrentBase, CurrentTip))
	{
		return;
	}
	if (!bHasPrevWeaponPose)
	{
		PrevWeaponBase = CurrentBase;
		PrevWeaponTip = CurrentTip;
		bHasPrevWeaponPose = true;
	}

	// Acteurs attachés au propriétaire (arme dégainée, bouclier, monture...) : ignorés par les balayages.
	// Calculé une fois par tick d'animation, dans un tableau réutilisé (pas d'allocation par sous-pas).
	ScratchAttached.Reset();
	if (const AActor* Owner = GetOwner())
	{
		Owner->GetAttachedActors(ScratchAttached);
	}

	const float Radius = FMath::Max(1.f, ActiveHitParams.SphereRadius);
	const float Travel = FMath::Max(FVector::Dist(PrevWeaponBase, CurrentBase), FVector::Dist(PrevWeaponTip, CurrentTip));
	const int32 SubSteps = FMath::Clamp(FMath::CeilToInt(Travel / (Radius * SubStepTravelFactor)), 1, MaxHitSubSteps);

	FVector Motion = CurrentTip - PrevWeaponTip;
	if (!Motion.Normalize())
	{
		Motion = GetOwner() ? GetOwner()->GetActorForwardVector() : FVector::ForwardVector;
	}

	for (int32 Step = 1; Step <= SubSteps; ++Step)
	{
		const float Alpha = static_cast<float>(Step) / static_cast<float>(SubSteps);
		SweepBlade(
			FMath::Lerp(PrevWeaponBase, CurrentBase, Alpha),
			FMath::Lerp(PrevWeaponTip, CurrentTip, Alpha),
			Motion);
	}

	PrevWeaponBase = CurrentBase;
	PrevWeaponTip = CurrentTip;
}

void UEMBCombatComponent::EndHitWindow()
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	bHitWindowActive = false;
	HitActorsThisWindow.Reset();

	// Fin de la zone active -> Recovery (sauf si l'attaque a déjà été interrompue : étourdissement, mort...).
	if (IsAttackState(CurrentState) && !(CurrentState == EEMBCombatState::ChargedAttack && bCharging))
	{
		TransitionTo(EEMBCombatState::Recovery);
	}
}

void UEMBCombatComponent::SweepBlade(const FVector& Base, const FVector& Tip, const FVector& MotionDirection)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(EMBMeleeSweep), /*bTraceComplex=*/false, GetOwner());
	QueryParams.AddIgnoredActors(ScratchAttached);

	/*
	 * Balayage multiple sur le canal EMBMeleeHit : pawns et Physics Assets y sont en Overlap, donc tous les
	 * corps croisés sont rapportés (aucun n'arrête le balayage) ; seul le décor en Block le termine, ce qui
	 * protège une cible derrière un mur.
	 */
	ScratchImpacts.Reset();
	World->SweepMultiByChannel(
		ScratchImpacts, Base, Tip, FQuat::Identity, HitTraceChannel.GetValue(),
		FCollisionShape::MakeSphere(FMath::Max(1.f, ActiveHitParams.SphereRadius)), QueryParams);

	// Un acteur peut être croisé par plusieurs corps (os) du même balayage : on garde le meilleur impact,
	// celui qui renseigne un os (points faibles des boss) étant prioritaire sur un impact sans os.
	ScratchBestImpact.Reset();
	for (int32 Index = 0; Index < ScratchImpacts.Num(); ++Index)
	{
		const FHitResult& Impact = ScratchImpacts[Index];
		AActor* Target = Impact.GetActor();
		if (!Target || Target == GetOwner() || Target->IsAttachedTo(GetOwner())
			|| (!bAllowFriendlyFire && IsFriendlyTarget(Target)))
		{
			continue;
		}

		const int32* ExistingIndex = ScratchBestImpact.Find(Target);
		if (!ExistingIndex)
		{
			ScratchBestImpact.Add(Target, Index);
		}
		else if (ScratchImpacts[*ExistingIndex].BoneName == NAME_None && Impact.BoneName != NAME_None)
		{
			ScratchBestImpact[Target] = Index;
		}
	}

	for (const TPair<AActor*, int32>& Entry : ScratchBestImpact)
	{
		AActor* Target = Entry.Key;

		// Déduplication : un acteur n'est touché qu'une fois par fenêtre de coup.
		bool bAlreadyHit = false;
		HitActorsThisWindow.Add(TWeakObjectPtr<AActor>(Target), &bAlreadyHit);
		if (bAlreadyHit)
		{
			continue;
		}

		FEMBHitData Data = BuildHitData(ScratchImpacts[Entry.Value], MotionDirection);

		// Riposte : le premier coup sur l'acteur paré pendant la fenêtre de 1,5 s est critique (x3).
		const bool bRiposte = IsRiposteWindowOpen() && RiposteTarget.Get() == Target;
		if (bRiposte)
		{
			/*
			 * Arbitrage : les multiplicateurs critiques ne se cumulent pas. Un boss étourdi applique déjà son
			 * propre critique x3 (riposte sur posture brisée) : si la cible porte State.Combat.Staggered, on
			 * n'ajoute pas le x3 de riposte (sinon x9). La fenêtre de riposte est consommée dans tous les cas.
			 * Cible sans IGameplayTagAssetInterface : comportement par défaut, le x3 s'applique.
			 */
			bool bTargetStaggered = false;
			if (const IGameplayTagAssetInterface* TagOwner = Cast<IGameplayTagAssetInterface>(Target))
			{
				bTargetStaggered = TagOwner->HasMatchingGameplayTag(EMBTags::State_Combat_Staggered.GetTag());
			}
			if (!bTargetStaggered)
			{
				// Passif "Riposte sanglante" : +0,5 par rang (x3 -> x3,5).
				Data.Damage *= EMBCombatDefaults::CriticalRiposteMultiplier + PassiveModifiers.RiposteMultiplier;
			}
		}

		const EEMBHitResult Result = ApplyHitToActor(Target, Data);

		if (bRiposte)
		{
			// La fenêtre se referme : une seule riposte par parade.
			LastParryTime = -1.0;
			RiposteTarget.Reset();
		}
		if (Result != EEMBHitResult::Ignored)
		{
			Client_ConfirmHit(Target, Result, Data.HitLocation);
		}
	}
}

void UEMBCombatComponent::Client_ConfirmHit_Implementation(AActor* Target, EEMBHitResult Result, FVector Location)
{
	// Exécuté sur le client propriétaire de l'attaquant (ou localement sur l'hôte) : sert à l'UI et aux FX.
	OnHitConfirmed.Broadcast(Target, Result, Location);
}

FEMBHitData UEMBCombatComponent::BuildHitData(const FHitResult& Impact, const FVector& MotionDirection) const
{
	FEMBHitData Data;
	Data.Instigator = GetOwner();
	Data.Damage = WeaponBaseDamage * ActiveAttack.DamageMultiplier * ActiveHitParams.DamageScale;
	if (GetWeaponDamageType() == EEMBDamageType::Physical)
	{
		Data.Damage *= 1.f + PassiveModifiers.PhysicalDamage; // Passif "Poigne d'acier" : +4 % par rang.
	}
	Data.PoiseDamage = ActiveAttack.PoiseDamage * ActiveHitParams.PoiseScale;
	Data.StaminaDamage = 0.f; // 0 => Damage x 0,5 côté cible.
	Data.DamageType = GetWeaponDamageType();
	Data.HitLocation = Impact.bStartPenetrating ? FVector(Impact.Location) : FVector(Impact.ImpactPoint);
	Data.HitDirection = MotionDirection;
	Data.HitBoneName = Impact.BoneName;
	Data.AttackTag = ActiveAttack.AttackTag;
	return Data;
}

bool UEMBCombatComponent::GetWeaponSocketLocations(FVector& OutBase, FVector& OutTip) const
{
	const USceneComponent* Weapon = GetWeaponComponent();
	if (!Weapon || !Weapon->DoesSocketExist(WeaponTraceStartSocket) || !Weapon->DoesSocketExist(WeaponTraceEndSocket))
	{
		return false;
	}
	OutBase = Weapon->GetSocketLocation(WeaponTraceStartSocket);
	OutTip = Weapon->GetSocketLocation(WeaponTraceEndSocket);
	return true;
}

USceneComponent* UEMBCombatComponent::GetWeaponComponent() const
{
	if (UMeshComponent* Weapon = WeaponMesh.Get())
	{
		return Weapon;
	}
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	return Character ? Character->GetMesh() : nullptr;
}

bool UEMBCombatComponent::IsFriendlyTarget(const AActor* Target) const
{
	// Coop : deux pawns contrôlés par des joueurs sont alliés. (Une vraie gestion d'équipes passera par
	// IGenericTeamAgentInterface si le PvP est ajouté.)
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const APawn* TargetPawn = Cast<APawn>(Target);
	return OwnerPawn && TargetPawn && OwnerPawn->IsPlayerControlled() && TargetPawn->IsPlayerControlled();
}

// ================================================================================================
// Résolution des coups reçus (serveur)
// ================================================================================================

EEMBHitResult UEMBCombatComponent::ApplyHitToActor(AActor* Target, const FEMBHitData& Hit)
{
	if (!IsValid(Target) || !Target->HasAuthority())
	{
		return EEMBHitResult::Ignored; // Les dégâts ne se résolvent que sur le serveur.
	}

	// 1) La cible traite elle-même le coup (boss : points faibles, invulnérabilité de phase).
	if (IEMBHitReceiver* Receiver = Cast<IEMBHitReceiver>(Target))
	{
		return Receiver->ReceiveEMBHit(Hit);
	}

	// 2) Cible avec FSM de combat (joueur).
	if (UEMBCombatComponent* Combat = Target->FindComponentByClass<UEMBCombatComponent>())
	{
		return Combat->ReceiveHit(Hit);
	}

	// 3) Cible avec simples jauges (ennemis ordinaires, créatures).
	if (UEMBStatsComponent* TargetStats = Target->FindComponentByClass<UEMBStatsComponent>())
	{
		if (TargetStats->IsDead())
		{
			return EEMBHitResult::Ignored;
		}
		TargetStats->ApplyDamage(Hit.Damage, Hit.Instigator);
		if (TargetStats->IsDead())
		{
			return EEMBHitResult::Killed;
		}
		if (!Hit.bDamageOverTime)
		{
			TargetStats->ApplyPoiseDamage(Hit.PoiseDamage, Hit.Instigator);
		}
		return EEMBHitResult::Hit;
	}

	// 4) Rien à frapper.
	return EEMBHitResult::Ignored;
}

EEMBHitResult UEMBCombatComponent::ReceiveHit(const FEMBHitData& IncomingHit)
{
	if (!HasAuthorityOwner() || !Stats || !GetWorld())
	{
		return EEMBHitResult::Ignored;
	}
	if (CurrentState == EEMBCombatState::Dead || Stats->IsDead() || IncomingHit.Instigator == GetOwner())
	{
		return EEMBHitResult::Ignored;
	}

	// Copie modifiable : la résistance au feu réduit les dégâts avant toute autre règle (garde, parade,
	// dégâts continus), et le reste de la résolution travaille sur la valeur réduite.
	FEMBHitData Hit = IncomingHit;

	// Le drapeau de zone létale est levé pendant TOUTE la résolution, puis rétabli à la sortie (quel que soit
	// le return) : OnDeath, donc HandleOwnerDeath, est diffusé à l'intérieur d'ApplyDamage.
	TGuardValue<bool> LethalZoneGuard(bProcessingLethalZone, Hit.bLethalZone);

	if (Hit.DamageType == EEMBDamageType::Fire)
	{
		Hit.Damage *= 1.f - GetFireResistance();
	}

	// Dégâts continus (lave, brûlure, zones de feu) : dégâts directs. Ni esquive, ni garde, ni parade,
	// ni counter-hit, ni multicast (ces dégâts tombent à haute fréquence : pas de retour de coup réseau).
	if (Hit.bDamageOverTime)
	{
		// Seule l'invulnérabilité de réapparition les arrête : sinon un joueur qui réapparaît dans la lave
		// mourrait avant d'avoir pu bouger. Les i-frames de roulade, elles, ne protègent pas des dégâts continus.
		if (bSpawnProtected)
		{
			return EEMBHitResult::Dodged;
		}
		Stats->ApplyDamage(Hit.Damage, Hit.Instigator);
		return Stats->IsDead() ? EEMBHitResult::Killed : EEMBHitResult::Hit;
	}

	const EEMBHitResult Result = ResolveHit(Hit);
	if (Result != EEMBHitResult::Ignored)
	{
		// Retour visuel/sonore chez tous les clients ; OnHitReceived n'est diffusé que par ce multicast
		// (le serveur l'exécute aussi localement), donc jamais deux fois.
		Multicast_BroadcastHitReceived(Hit, Result);
	}
	return Result;
}

EEMBHitResult UEMBCombatComponent::ResolveHit(const FEMBHitData& Hit)
{
	// 1) Esquive : i-frames (State.Combat.Invulnerable).
	if (IsInvulnerable())
	{
		return EEMBHitResult::Dodged;
	}

	const bool bFacing = IsFacingAttacker(Hit);

	// 2) Parade parfaite : fenêtre de 10 frames, de face, attaque parable.
	if (CurrentState == EEMBCombatState::Parry && bParryWindowOpen && !Hit.bUnparryable && bFacing)
	{
		return ResolvePerfectParry(Hit);
	}

	// 3) Garde : levée depuis 0,10 s, de face, attaque blocable.
	if (CurrentState == EEMBCombatState::Block
		&& GetWorld()->GetTimeSeconds() >= BlockGuardUpTime
		&& !Hit.bUnblockable && bFacing)
	{
		return ResolveBlockedHit(Hit);
	}

	// 4) Coup reçu en pleine face (y compris parade ratée, recovery, startup).
	return ResolveUnguardedHit(Hit);
}

EEMBHitResult UEMBCombatComponent::ResolvePerfectParry(const FEMBHitData& Hit)
{
	LastParryTime = GetWorld()->GetTimeSeconds();
	RiposteTarget = Hit.Instigator.Get();
	bParryLanded = true; // La fin de fenêtre ne doit pas transformer cette parade réussie en parade "ratée".

	ApplyParryPosture(Hit.Instigator);

	Multicast_ParrySuccess(Hit.Instigator);
	return EEMBHitResult::Parried;
}

void UEMBCombatComponent::ApplyParryPosture(AActor* Attacker)
{
	if (!IsValid(Attacker))
	{
		return;
	}

	// Si l'attaquant traite lui-même ses coups (boss), on lui envoie un coup de posture pur : il décide
	// (étourdissement, invulnérabilité de phase...). Damage = 0, AttackTag vide, Instigator = ce joueur.
	if (IEMBHitReceiver* Receiver = Cast<IEMBHitReceiver>(Attacker))
	{
		FEMBHitData PostureHit;
		PostureHit.Instigator = GetOwner();
		PostureHit.Damage = 0.f;
		PostureHit.PoiseDamage = EMBCombatDefaults::ParryPostureDamage;
		PostureHit.HitLocation = Attacker->GetActorLocation();
		PostureHit.HitDirection = (Attacker->GetActorLocation() - GetOwner()->GetActorLocation()).GetSafeNormal();
		Receiver->ReceiveEMBHit(PostureHit);
		return;
	}

	// Ennemi ordinaire : chemin direct sur ses jauges (pas de StatsComponent = pas déstabilisé).
	if (UEMBStatsComponent* AttackerStats = Attacker->FindComponentByClass<UEMBStatsComponent>())
	{
		AttackerStats->ApplyPoiseDamage(EMBCombatDefaults::ParryPostureDamage, GetOwner());
	}
}

EEMBHitResult UEMBCombatComponent::ResolveBlockedHit(const FEMBHitData& Hit)
{
	// Coût d'endurance du coup bloqué : StaminaDamage, ou Damage x 0,5 si non renseigné.
	const float StaminaCost = Hit.StaminaDamage > 0.f ? Hit.StaminaDamage : Hit.Damage * 0.5f;
	// Passif "Garde inébranlable" : -15 % par rang sur le coût d'endurance des coups bloqués.
	const bool bHadStamina = Stats->ConsumeStamina(StaminaCost * FMath::Max(0.f, 1.f + PassiveModifiers.GuardStaminaCost));

	// Dégâts résiduels : le complément de la réduction (physique 85 %, feu/givre/foudre 50 %, corruption 30 %).
	Stats->ApplyDamage(Hit.Damage * (1.f - GetBlockReduction(Hit.DamageType)), Hit.Instigator);
	if (Stats->IsDead())
	{
		return EEMBHitResult::Killed;
	}

	// Rupture de garde : endurance déjà vide, ou tombée à 0 sur ce coup -> étourdi 1,2 s.
	if (!bHadStamina || !Stats->CanAffordStamina())
	{
		EnterStaggered(EMBCombatDefaults::GuardBreakStaggerDuration, true);
		return EEMBHitResult::GuardBroken;
	}
	return EEMBHitResult::Blocked;
}

EEMBHitResult UEMBCombatComponent::ResolveUnguardedHit(const FEMBHitData& Hit)
{
	float Damage = Hit.Damage;
	float PoiseDamage = Hit.PoiseDamage;

	// Counter-hit : frappé en Recovery, pendant le startup d'une attaque ou après une parade ratée.
	if (IsVulnerableToCounterHit())
	{
		Damage *= EMBCombatDefaults::CounterHitDamageMultiplier;
	}

	// Hyper-armure : la poise subie est réduite de moitié.
	if (HasStateTag(EMBTags::State_Combat_HyperArmor.GetTag()))
	{
		PoiseDamage *= EMBCombatDefaults::HyperArmorPoiseMultiplier;
	}

	Stats->ApplyDamage(Damage, Hit.Instigator); // Peut déclencher OnDeath -> état Dead (HandleOwnerDeath).
	if (Stats->IsDead())
	{
		return EEMBHitResult::Killed;
	}

	// Peut déclencher OnPoiseBroken -> état Staggered (HandleOwnerPoiseBroken).
	Stats->ApplyPoiseDamage(PoiseDamage, Hit.Instigator);
	return EEMBHitResult::Hit;
}

bool UEMBCombatComponent::IsFacingAttacker(const FEMBHitData& Hit) const
{
	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}

	// Direction vers l'attaquant (plus fiable que HitDirection pour les attaques de zone et de souffle) ;
	// à défaut d'instigateur, on retourne la direction du coup.
	FVector ToAttacker = Hit.Instigator
		? Hit.Instigator->GetActorLocation() - Owner->GetActorLocation()
		: -Hit.HitDirection;
	ToAttacker.Z = 0.f;
	if (!ToAttacker.Normalize())
	{
		return true; // Superposés : on considère le coup de face.
	}

	FVector Forward = Owner->GetActorForwardVector();
	Forward.Z = 0.f;
	Forward.Normalize();

	const float MinDot = FMath::Cos(FMath::DegreesToRadians(GuardHalfAngleDegrees));
	return FVector::DotProduct(Forward, ToAttacker) >= MinDot;
}

bool UEMBCombatComponent::IsVulnerableToCounterHit() const
{
	switch (CurrentState)
	{
	case EEMBCombatState::Recovery:
		return true;
	case EEMBCombatState::LightAttack:
	case EEMBCombatState::HeavyAttack:
	case EEMBCombatState::ChargedAttack:
		return !bHitWindowStarted; // Startup : la fenêtre active n'a pas encore commencé.
	case EEMBCombatState::Parry:
		return bParryWhiffed;
	default:
		return false;
	}
}

void UEMBCombatComponent::Multicast_BroadcastHitReceived_Implementation(const FEMBHitData& Hit, EEMBHitResult Result)
{
	OnHitReceived.Broadcast(Hit, Result);
}

void UEMBCombatComponent::Multicast_ParrySuccess_Implementation(AActor* Attacker)
{
	OnParrySuccess.Broadcast(Attacker);
}

// ================================================================================================
// Montages
// ================================================================================================

UAnimInstance* UEMBCombatComponent::GetOwnerAnimInstance() const
{
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	const USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	return Mesh ? Mesh->GetAnimInstance() : nullptr;
}

float UEMBCombatComponent::GetDodgeIFrameEnd() const
{
	// Fin des i-frames selon la charge portée : 13 / 11 / 9 frames (début commun à la frame 3).
	float End = EMBCombatDefaults::DodgeIFrameEndHeavy;
	if (EncumbranceRatio < EMBCombatDefaults::LoadLightMax)
	{
		End = EMBCombatDefaults::DodgeIFrameEndLight;
	}
	else if (EncumbranceRatio < EMBCombatDefaults::LoadMediumMax)
	{
		End = EMBCombatDefaults::DodgeIFrameEndMedium;
	}
	// Passif "Pas fantôme" : +2 frames d'i-frames par rang.
	return End + FMath::Max(0.f, PassiveModifiers.DodgeIFrames) * EMBCombatDefaults::FrameTime;
}

void UEMBCombatComponent::SetPassiveModifiers(const FEMBPassiveModifiers& NewModifiers)
{
	PassiveModifiers = NewModifiers;
}

float UEMBCombatComponent::ScaledAttackCost(float BaseCost) const
{
	// Passif "Souffle du duelliste" : -5 % par rang (additif négatif), jamais en dessous de 0.
	return BaseCost * FMath::Max(0.f, 1.f + PassiveModifiers.AttackStaminaCost);
}

float UEMBCombatComponent::GetBlockReduction(EEMBDamageType DamageType)
{
	switch (DamageType)
	{
	case EEMBDamageType::Physical:   return EMBCombatDefaults::BlockReductionPhysical;
	case EEMBDamageType::Corruption: return EMBCombatDefaults::BlockReductionCorruption;
	default:                         return EMBCombatDefaults::BlockReductionElemental; // feu, givre, foudre
	}
}

UAnimMontage* UEMBCombatComponent::GetDodgeMontage() const
{
	UAnimMontage* Montage = DodgeMontageHeavy;
	if (EncumbranceRatio < EMBCombatDefaults::LoadLightMax)
	{
		Montage = DodgeMontageLight;
	}
	else if (EncumbranceRatio < EMBCombatDefaults::LoadMediumMax)
	{
		Montage = DodgeMontageMedium;
	}
	return Montage ? Montage : DodgeMontageLight.Get();
}

bool UEMBCombatComponent::PlayActionMontage(UAnimMontage* Montage, float PlayRate, bool bTrackEnd, bool bReliable)
{
	// Mémorise le montage dont la fin pilote la FSM : Multicast_PlayMontage (exécuté aussi sur le serveur)
	// s'en sert pour poser le délégué de fin.
	if (bTrackEnd)
	{
		ActiveMontage = Montage;
	}
	else
	{
		ActiveMontage.Reset();
	}

	bServerMontageStarted = false;
	if (!Montage)
	{
		return false;
	}

	const float SafeRate = FMath::Max(PlayRate, KINDA_SMALL_NUMBER);
	// Fiable pour ce dont la perte se voit (mort, étourdissement, garde et charge en boucle) ; non fiable
	// pour les attaques et roulades, où un paquet perdu n'est qu'un défaut visuel passager.
	if (bReliable)
	{
		Multicast_PlayMontageReliable(Montage, SafeRate, NAME_None);
	}
	else
	{
		Multicast_PlayMontage(Montage, SafeRate, NAME_None);
	}

	if (!bServerMontageStarted)
	{
		return false; // Pas d'AnimInstance (ou montage invalide) sur le serveur : mode dégradé par temporisateurs.
	}

	if (bTrackEnd)
	{
		// Filet de sécurité : si le délégué de fin ne partait jamais (animation non évaluée sur un
		// serveur dédié, par exemple), la FSM ne doit pas rester bloquée dans l'action.
		ScheduleActionTimer(Montage->GetPlayLength() / SafeRate + MontageFailsafeMargin, [this]()
		{
			OnActionFinished();
		});
	}
	return true;
}

bool UEMBCombatComponent::PlayMontageLocal(UAnimMontage* Montage, float PlayRate, FName StartSection, bool bTrackEnd)
{
	UAnimInstance* Anim = GetOwnerAnimInstance();
	if (!Anim || !Montage)
	{
		return false;
	}

	if (Anim->Montage_Play(Montage, PlayRate) <= 0.f)
	{
		return false;
	}
	if (StartSection != NAME_None)
	{
		Anim->Montage_JumpToSection(StartSection, Montage);
	}

	if (bTrackEnd)
	{
		/*
		 * Fin de l'action pilotée par le DÉBUT du blend-out du montage (Montage_SetBlendingOutDelegate) et non
		 * par FOnMontageEnded, qui n'arrive qu'à la fin complète du fondu : le personnage redevient libre
		 * (Idle, buffer consommé) dès que l'animation commence à se fondre, sans attendre jusqu'au temps de
		 * blend-out du montage en plus (0,25 s par défaut). Les animateurs règlent donc le BlendOut du montage
		 * pour fixer la cadence de reprise. Le délégué part aussi en cas d'interruption ; le jeton écarte les
		 * événements périmés (montage remplacé par une nouvelle action, par exemple deux roulades de suite).
		 */
		const uint32 Token = ActionToken;
		FOnMontageBlendingOutStarted BlendOutDelegate;
		BlendOutDelegate.BindWeakLambda(this, [this, Token](UAnimMontage* /*BlendingMontage*/, bool /*bInterrupted*/)
		{
			if (Token == ActionToken)
			{
				OnActionFinished();
			}
		});
		Anim->Montage_SetBlendingOutDelegate(BlendOutDelegate, Montage);
	}
	return true;
}

void UEMBCombatComponent::Multicast_PlayMontage_Implementation(UAnimMontage* Montage, float PlayRate, FName StartSection)
{
	HandleMulticastPlayMontage(Montage, PlayRate, StartSection);
}

void UEMBCombatComponent::Multicast_PlayMontageReliable_Implementation(UAnimMontage* Montage, float PlayRate, FName StartSection)
{
	HandleMulticastPlayMontage(Montage, PlayRate, StartSection);
}

void UEMBCombatComponent::HandleMulticastPlayMontage(UAnimMontage* Montage, float PlayRate, FName StartSection)
{
	if (!Montage)
	{
		return;
	}

	if (HasAuthorityOwner())
	{
		const bool bTrackEnd = ActiveMontage.Get() == Montage;
		bServerMontageStarted = PlayMontageLocal(Montage, PlayRate, StartSection, bTrackEnd);
		return;
	}

	/*
	 * Client. Compromis de réactivité : le propriétaire a déjà lancé ce montage en prédiction dans
	 * HandleInput. S'il tourne toujours, on ignore le multicast (sinon l'animation repartirait de zéro
	 * avec le retard du réseau). Pas de rollback complet : si le serveur refuse l'action, il envoie
	 * Client_RejectPrediction qui coupe le montage prédit. Les autres clients (proxies simulés) jouent
	 * simplement le montage à réception ; le multicast étant Unreliable, un paquet perdu = une animation
	 * manquante côté visuel seulement, l'état de gameplay (CurrentState) étant répliqué à part.
	 */
	const UAnimInstance* Anim = GetOwnerAnimInstance();
	if (Anim && Anim->Montage_IsPlaying(Montage))
	{
		PredictedMontage.Reset();
		return;
	}
	PlayMontageLocal(Montage, PlayRate, StartSection, false);
}

void UEMBCombatComponent::Multicast_StopMontage_Implementation(UAnimMontage* Montage, float BlendOutTime)
{
	// Arrêt ciblé : si le montage n'est plus en lecture (remplacé par une autre action), c'est sans effet.
	UAnimInstance* Anim = GetOwnerAnimInstance();
	if (Anim && Montage && Anim->Montage_IsPlaying(Montage))
	{
		Anim->Montage_Stop(BlendOutTime, Montage);
	}
}

void UEMBCombatComponent::Client_RejectPrediction_Implementation()
{
	UAnimInstance* Anim = GetOwnerAnimInstance();
	UAnimMontage* Montage = PredictedMontage.Get();
	if (Anim && Montage && Anim->Montage_IsPlaying(Montage))
	{
		Anim->Montage_Stop(0.1f, Montage);
	}
	PredictedMontage.Reset();
}

void UEMBCombatComponent::PredictLocally(EEMBCombatInput Input)
{
	UAnimInstance* Anim = GetOwnerAnimInstance();
	if (!Anim || !Stats || !Stats->CanAffordStamina())
	{
		return;
	}

	// Prédiction prudente : uniquement quand l'état répliqué laisse clairement agir, pour que le
	// serveur donne presque toujours raison au client.
	const bool bFree = CurrentState == EEMBCombatState::Idle || CurrentState == EEMBCombatState::Block;
	const bool bInComboWindow = CurrentState == EEMBCombatState::Recovery && bComboWindowOpen; // bComboWindowOpen : notify local

	UAnimMontage* Montage = nullptr;
	float PlayRate = 1.f;

	switch (Input)
	{
	case EEMBCombatInput::LightAttack:
	{
		int32 NextIndex = -1;
		if (bFree)
		{
			NextIndex = 0;
		}
		else if (bInComboWindow && static_cast<int32>(ComboIndex) + 1 < LightComboAttacks.Num())
		{
			NextIndex = static_cast<int32>(ComboIndex) + 1;
		}
		if (LightComboAttacks.IsValidIndex(NextIndex))
		{
			Montage = LightComboAttacks[NextIndex].Montage;
			PlayRate = LightComboAttacks[NextIndex].PlayRate;
		}
		break;
	}
	case EEMBCombatInput::HeavyAttack:
		if (bFree || bInComboWindow)
		{
			Montage = HeavyAttack.Montage;
			PlayRate = HeavyAttack.PlayRate;
		}
		break;
	case EEMBCombatInput::ChargedAttackStart:
		if (bFree)
		{
			Montage = ChargeLoopMontage;
		}
		break;
	case EEMBCombatInput::Dodge:
		if (bFree && EncumbranceRatio <= EMBCombatDefaults::LoadHeavyMax)
		{
			Montage = GetDodgeMontage();
		}
		break;
	case EEMBCombatInput::Parry:
		if (bFree)
		{
			Montage = ParryMontage;
		}
		break;
	default:
		break; // Relâchement de charge, garde : aucune prédiction (états maintenus / résolus par le serveur).
	}

	if (Montage)
	{
		PredictedMontage = Montage;
		Anim->Montage_Play(Montage, FMath::Max(PlayRate, KINDA_SMALL_NUMBER));
	}
}

// ================================================================================================
// Temporisateurs d'action
// ================================================================================================

void UEMBCombatComponent::ScheduleActionTimer(float Delay, TFunction<void()> Callback)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const uint32 Token = ActionToken;
	FTimerHandle& Handle = ActionTimers.AddDefaulted_GetRef();
	World->GetTimerManager().SetTimer(
		Handle,
		FTimerDelegate::CreateWeakLambda(this, [this, Token, Callback = MoveTemp(Callback)]()
		{
			// Le jeton protège contre un temporisateur qui survivrait à son action.
			if (Token == ActionToken)
			{
				Callback();
			}
		}),
		FMath::Max(Delay, KINDA_SMALL_NUMBER), // Un délai <= 0 annulerait le timer.
		false);
}

void UEMBCombatComponent::ClearActionTimers()
{
	if (const UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		for (FTimerHandle& Handle : ActionTimers)
		{
			TimerManager.ClearTimer(Handle);
		}
	}
	ActionTimers.Reset();
}

void UEMBCombatComponent::ScheduleFallbackHitWindow(float Start, float End)
{
	// Mode dégradé (aucun montage) : pas de balayage continu possible, on teste la lame au début et à la
	// fin de la fenêtre. Sert au prototypage ; les montages réels pilotent via EMBAnimNotifyState_HitWindow.
	ScheduleActionTimer(Start, [this]()
	{
		BeginHitWindow(FEMBHitWindowParams());
		TickHitWindow(0.f);
	});
	ScheduleActionTimer(End, [this]()
	{
		TickHitWindow(0.f);
		EndHitWindow();
	});
}

void UEMBCombatComponent::ScheduleStateTagWindow(float Start, float End, bool bHyperArmor)
{
	const FGameplayTag Tag = bHyperArmor
		? EMBTags::State_Combat_HyperArmor.GetTag()
		: EMBTags::State_Combat_Invulnerable.GetTag();

	ScheduleActionTimer(Start, [this, Tag]() { BeginStateTag(Tag); });
	ScheduleActionTimer(End, [this, Tag]() { EndStateTag(Tag); });
}

void UEMBCombatComponent::ScheduleComboWindowFallback(float Open, float Close)
{
	ScheduleActionTimer(Open, [this]() { SetComboWindowOpen(true); });
	ScheduleActionTimer(Close, [this]() { SetComboWindowOpen(false); });
}

void UEMBCombatComponent::ScheduleFallbackTimeline(EEMBCombatState State)
{
	switch (State)
	{
	case EEMBCombatState::LightAttack:
		ScheduleFallbackHitWindow(EMBCombatDefaults::LightActiveStart, EMBCombatDefaults::LightActiveEnd);
		ScheduleComboWindowFallback(EMBCombatDefaults::LightComboWindowOpen, EMBCombatDefaults::LightComboWindowClose);
		ScheduleActionTimer(EMBCombatDefaults::LightTotalDuration, [this]() { OnActionFinished(); });
		break;

	case EEMBCombatState::HeavyAttack:
		ScheduleFallbackHitWindow(EMBCombatDefaults::HeavyActiveStart, EMBCombatDefaults::HeavyActiveEnd);
		ScheduleActionTimer(EMBCombatDefaults::HeavyTotalDuration, [this]() { OnActionFinished(); });
		break;

	case EEMBCombatState::ChargedAttack:
		ScheduleFallbackHitWindow(EMBCombatDefaults::ChargedStrikeActiveStart, EMBCombatDefaults::ChargedStrikeActiveEnd);
		ScheduleActionTimer(EMBCombatDefaults::ChargedStrikeDuration, [this]() { OnActionFinished(); });
		break;

	case EEMBCombatState::Dodge:
	{
		// Les i-frames sont déjà planifiées par EnterState ; il ne reste que la fin de l'action.
		ScheduleActionTimer(EMBCombatDefaults::DodgeTotalDuration, [this]() { OnActionFinished(); });
		break;
	}

	case EEMBCombatState::Parry:
		ScheduleActionTimer(EMBCombatDefaults::ParryTotalDuration, [this]() { OnActionFinished(); });
		break;

	default:
		break;
	}
}
