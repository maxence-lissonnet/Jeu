// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBColossusBoss.h"

#include "Emberfall.h"
#include "EMBBossTags.h"
#include "EMBColossusArena.h"
#include "EMBCombatComponent.h"
#include "EMBGameplayTags.h"
#include "EMBStatsComponent.h"

#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"

namespace
{
	/** Nom de l'os du noyau runique (dans le dos du Colosse) : point faible conditionnel. */
	const FName CoreBoneName(TEXT("core"));

	/** Rayon (cm) du flash visuel d'impact de la charge. */
	constexpr float ChargeImpactFlashRadius = 300.f;
	constexpr float ChargeImpactFlashDuration = 0.4f;
}

AEMBColossusBoss::AEMBColossusBoss()
{
	// PV 12 000, posture 600, régénération 25/s après 3 s (section 11.2). Le contrôleur par défaut (AEMBBossAIController)
	// est celui de la base : le Blueprint du Colosse y assigne BT_Colossus et BB_Colossus.
	ConfigureBossStats(EMBColossusDefaults::MaxHealthSolo, EMBColossusDefaults::MaxPoise,
		EMBColossusDefaults::PoiseRegenRate, EMBColossusDefaults::PoiseRegenDelay);

	// Deux phases : P2 à 50 % des PV ; transition invulnérable de 4 s ; étourdi 4 s.
	PhaseHealthThresholds = { EMBColossusDefaults::Phase2HealthThreshold };
	PhaseTransitionDuration = EMBColossusDefaults::PhaseTransitionDuration;
	StunDuration = EMBColossusDefaults::StunDuration;

	// P2 "runes embrasées" : seuls les télégraphes sont raccourcis (x0,9) ; vitesse des montages et recharges inchangées.
	EnrageFromPhase = 2;
	EnragedPlayRate = 1.f;
	EnragedCooldownMultiplier = 1.f;
	EnragedTelegraphMultiplier = EMBColossusDefaults::EnragedTelegraphMultiplier;

	// Résistances : physique 10 %, feu 40 %, foudre -20 % (faiblesse), corruption 0 %.
	DamageResistances.Add(EEMBDamageType::Physical, EMBColossusDefaults::ResistPhysical);
	DamageResistances.Add(EEMBDamageType::Fire, EMBColossusDefaults::ResistFire);
	DamageResistances.Add(EEMBDamageType::Lightning, EMBColossusDefaults::ResistLightning);
	DamageResistances.Add(EEMBDamageType::Corruption, EMBColossusDefaults::ResistCorruption);

	// Arène : cour circulaire de 50 m, rayon de rencontre 3 000 cm (maintien de la rencontre, abandon après un wipe).
	ArenaRadius = 3000.f;

	// Noyau runique dans le dos : x1,5 uniquement pendant l'exposition qui suit le Séisme (sinon x1,0).
	ConditionalWeakPointMultipliers.Add(CoreBoneName, EMBColossusDefaults::CoreMultiplier);

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = EMBColossusDefaults::WalkSpeed;
	}
}

FGameplayTag AEMBColossusBoss::GetDefeatedEventTag() const
{
	return EMBTags::Event_Boss_Killed_Guardian.GetTag();
}

bool AEMBColossusBoss::IsAttackAllowed(const FEMBBossAttackRow& Row) const
{
	// Résonance : poids 0 s'il ne reste aucun obélisque intact (rien à canaliser).
	if (Row.AttackTag == EMBBossTags::Attack_Guardian_Resonance.GetTag())
	{
		return GetIntactObeliskCount() > 0;
	}
	return true;
}

// ----------------------------------------------------------------------------------------------
// Arène et obélisques
// ----------------------------------------------------------------------------------------------

void AEMBColossusBoss::SetArena(AEMBColossusArena* InArena)
{
	Arena = InArena;
}

int32 AEMBColossusBoss::GetIntactObeliskCount() const
{
	const AEMBColossusArena* ArenaPtr = Arena.Get();
	return ArenaPtr ? ArenaPtr->GetIntactObeliskCount() : 0;
}

bool AEMBColossusBoss::IsValidChargeObstacle(const AActor* HitActor) const
{
	if (!HitActor)
	{
		return false;
	}
	const AEMBColossusArena* ArenaPtr = Arena.Get();
	return ArenaPtr ? ArenaPtr->IsChargeObstacle(HitActor) : true;
}

void AEMBColossusBoss::ExposeCore(const float Duration)
{
	SetConditionalWeakPointsActive(true, Duration);
}

// ----------------------------------------------------------------------------------------------
// Charge de bélier
// ----------------------------------------------------------------------------------------------

void AEMBColossusBoss::ResolveChargeImpact(const FHitResult& BlockingHit, const FEMBBossAttackRow& Row)
{
	if (!HasAuthority() || IsDead())
	{
		return;
	}

	// Un obélisque (ou tout acteur capable de subir un coup) encaisse la charge ; un mur ne fait rien.
	// (*) Les dégâts de la charge sur un obélisque sont ceux de la ligne de données (220) : à valider en playtest.
	AActor* HitActor = BlockingHit.GetActor();
	if (HitActor && HitActor != this && !HitActor->IsA<APawn>())
	{
		FEMBHitData ImpactHit = MakeHitData(Row);
		ImpactHit.HitLocation = BlockingHit.ImpactPoint;
		ImpactHit.HitDirection = GetActorForwardVector();
		UEMBCombatComponent::ApplyHitToActor(HitActor, ImpactHit);
	}

	Multicast_ShowGroundMarker(ProjectToGround(BlockingHit.ImpactPoint), ChargeImpactFlashRadius, ChargeImpactFlashDuration, true);

	// Le Colosse percute le décor : étourdi 3 s (riposte critique x3 pour les joueurs).
	BeginStun(EMBColossusDefaults::ChargeImpactStunDuration);
}

// ----------------------------------------------------------------------------------------------
// Hooks de la base
// ----------------------------------------------------------------------------------------------

bool AEMBColossusBoss::ScheduleCustomEvents(const FEMBBossAttackRow& Row, const float TimeScale, float& InOutLastEventTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	if (Row.Kind == EEMBDragonAttackKind::Rings)
	{
		// Ondes runiques : un timer par anneau (télégraphe, puis un anneau toutes les 0,8 s). Les anneaux déjà émis
		// se propagent seuls (RingTimerHandle) même si l'attaque est annulée ; ceux pas encore émis sont annulés.
		// Seul le télégraphe est réduit en phase 2 (x0,9) : l'espacement des anneaux reste exactement 0,8 s.
		float LastRingTime = Row.TelegraphTime * TimeScale;
		for (int32 RingIndex = 0; RingIndex < EMBColossusDefaults::RuneRingCount; ++RingIndex)
		{
			LastRingTime = Row.TelegraphTime * TimeScale + static_cast<float>(RingIndex) * EMBColossusDefaults::RuneRingInterval;
			AddAttackTimer(FTimerDelegate::CreateUObject(this, &AEMBColossusBoss::StartRuneRing, Row.AttackTag), LastRingTime);
		}
		// InOutLastEventTime est exprimé avant mise à l'échelle (la base le multiplie par TimeScale) : on le convertit.
		InOutLastEventTime = FMath::Max(InOutLastEventTime,
			(LastRingTime + EMBColossusDefaults::RuneRingRecoveryAfterLast) / FMath::Max(TimeScale, 0.01f));
		return true;
	}

	if (Row.Kind == EEMBDragonAttackKind::Channel)
	{
		// Résonance : canalisation de TelegraphTime secondes. Le timer a sa propre poignée pour pouvoir être
		// interrompu (posture reçue) sans toucher aux autres timers de l'attaque.
		bChannelingResonance = true;
		ResonancePoiseAccumulated = 0.f;
		// Durée de canalisation EXACTE (3,0 s) : elle n'est pas réduite par les télégraphes de phase 2. La base
		// multipliant InOutLastEventTime par TimeScale, on le convertit pour que l'attente du BT couvre bien 3,0 s.
		World->GetTimerManager().SetTimer(ResonanceTimerHandle, this, &AEMBColossusBoss::CompleteResonance,
			FMath::Max(Row.TelegraphTime, 0.05f), false);
		InOutLastEventTime = FMath::Max(InOutLastEventTime, Row.TelegraphTime / FMath::Max(TimeScale, 0.01f));
		return true;
	}

	return false;
}

void AEMBColossusBoss::OnTimedHitExecuted(const FEMBBossAttackRow& Row, const int32 HitIndex, const TArray<AActor*>& Landed)
{
	// Séisme : l'onde (rayon 800) est résolue par la base ; le Colosse expose son noyau 3 s, même si personne n'est touché.
	if (Row.AttackTag == EMBBossTags::Attack_Guardian_Quake.GetTag())
	{
		ExposeCore(EMBColossusDefaults::CoreExposureDuration);
		Multicast_ShowGroundMarker(GetActorLocation(), Row.MaxRange, ChargeImpactFlashDuration, true);
	}
}

FGameplayTag AEMBColossusBoss::GetChainedAttackTag(const FEMBBossAttackRow& Row) const
{
	// Rugissement du lion : enchaîne le Poing-marteau (le joueur dont la poise est brisée le reçoit de plein fouet).
	if (Row.AttackTag == EMBBossTags::Attack_Guardian_LionRoar.GetTag())
	{
		return EMBBossTags::Attack_Guardian_HammerFist.GetTag();
	}
	return FGameplayTag();
}

void AEMBColossusBoss::OnPoiseDamageTaken(const float Amount, AActor* InInstigator)
{
	// Résonance : 150 de posture reçue pendant la canalisation l'interrompent (aucun soin).
	if (!bChannelingResonance)
	{
		return;
	}
	ResonancePoiseAccumulated += Amount;
	if (ResonancePoiseAccumulated >= EMBColossusDefaults::ResonancePoiseInterrupt)
	{
		InterruptResonance();
	}
}

void AEMBColossusBoss::OnAttackCancelled()
{
	// Étourdissement, transition, mort ou nouvelle attaque pendant la canalisation : la Résonance est annulée.
	if (bChannelingResonance)
	{
		InterruptResonance();
	}
}

void AEMBColossusBoss::OnDefeatedServer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RingTimerHandle);
		World->GetTimerManager().ClearTimer(ResonanceTimerHandle);
	}
	ActiveRings.Reset();
	bChannelingResonance = false;
}

// ----------------------------------------------------------------------------------------------
// Ondes runiques
// ----------------------------------------------------------------------------------------------

void AEMBColossusBoss::StartRuneRing(FGameplayTag AttackTag)
{
	UWorld* World = GetWorld();
	const FEMBBossAttackRow* Row = FindAttackRow(AttackTag);
	if (!World || !Row || IsDead() || IsStunned() || IsTransitioning())
	{
		return;
	}

	FRuneRing& Ring = ActiveRings.AddDefaulted_GetRef();
	Ring.Center = GetActorLocation();
	Ring.StartTime = World->GetTimeSeconds();
	Ring.LastRadius = 0.f;
	Ring.MaxRadius = Row->MaxRange;
	Ring.HalfWidth = Row->EffectRadius * 0.5f; // épaisseur 150 -> bande de +/- 75 cm
	Ring.HitTemplate = MakeHitData(*Row);

	Multicast_RuneRing(Ring.Center, EMBColossusDefaults::RuneRingSpeed, Row->EffectRadius, Ring.MaxRadius);

	// Un seul timer répétitif (20 Hz) pour tous les anneaux en vol : jamais de Tick.
	FTimerManager& TimerManager = World->GetTimerManager();
	if (!TimerManager.IsTimerActive(RingTimerHandle))
	{
		TimerManager.SetTimer(RingTimerHandle, this, &AEMBColossusBoss::TickRuneRings, EMBColossusDefaults::RuneRingTickInterval, true);
	}
}

void AEMBColossusBoss::TickRuneRings()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	TArray<APawn*> Players;
	CollectAlivePlayerPawns(World, Players);

	for (int32 RingIndex = ActiveRings.Num() - 1; RingIndex >= 0; --RingIndex)
	{
		FRuneRing& Ring = ActiveRings[RingIndex];
		const float Radius = EMBColossusDefaults::RuneRingSpeed * static_cast<float>(Now - Ring.StartTime);

		if (Radius - Ring.HalfWidth > Ring.MaxRadius)
		{
			ActiveRings.RemoveAtSwap(RingIndex);
			continue;
		}

		for (APawn* Pawn : Players)
		{
			// Le joueur est touché quand sa distance au centre traverse le rayon courant (bande de +/- demi-épaisseur).
			// On teste l'intervalle parcouru depuis le dernier tick (LastRadius -> Radius) : aucun joueur n'est
			// "sauté" même si un hitch serveur allonge le pas.
			const float Distance = static_cast<float>(FVector::Dist2D(Pawn->GetActorLocation(), Ring.Center));
			if (Distance < Ring.LastRadius - Ring.HalfWidth || Distance > Radius + Ring.HalfWidth)
			{
				continue;
			}
			if (Ring.AlreadyHit.Contains(Pawn))
			{
				continue; // une seule fois par anneau et par joueur
			}
			Ring.AlreadyHit.Add(Pawn);

			FEMBHitData RingHit = Ring.HitTemplate;
			RingHit.Instigator = this;
			RingHit.HitLocation = Pawn->GetActorLocation();
			RingHit.HitDirection = (Pawn->GetActorLocation() - Ring.Center).GetSafeNormal2D();
			// Roulade à travers l'anneau (i-frames) : le composant de combat renvoie Dodged ; le joueur est marqué
			// comme traité dans tous les cas.
			UEMBCombatComponent::ApplyHitToActor(Pawn, RingHit);
		}

		Ring.LastRadius = Radius;
	}

	if (ActiveRings.Num() == 0)
	{
		World->GetTimerManager().ClearTimer(RingTimerHandle);
	}
}

void AEMBColossusBoss::Multicast_RuneRing_Implementation(const FVector Center, const float Speed, const float Thickness, const float MaxRadius)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnRuneRing(Center, Speed, Thickness, MaxRadius);
	}
}

// ----------------------------------------------------------------------------------------------
// Résonance des obélisques
// ----------------------------------------------------------------------------------------------

void AEMBColossusBoss::CompleteResonance()
{
	if (!bChannelingResonance)
	{
		return;
	}
	bChannelingResonance = false;

	float HealAmount = 0.f;
	if (StatsComponent && !IsDead())
	{
		// Soin de 3 % des PV max par obélisque intact au moment où la canalisation aboutit.
		const int32 IntactCount = GetIntactObeliskCount();
		HealAmount = StatsComponent->GetMaxHealth() * EMBColossusDefaults::ResonanceHealFractionPerObelisk * static_cast<float>(IntactCount);
		if (HealAmount > 0.f)
		{
			StatsComponent->Heal(HealAmount);
		}
	}
	Multicast_ResonanceFinished(true, HealAmount);
}

void AEMBColossusBoss::InterruptResonance()
{
	if (!bChannelingResonance)
	{
		return;
	}
	bChannelingResonance = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ResonanceTimerHandle);
	}
	Multicast_ResonanceFinished(false, 0.f);
}

void AEMBColossusBoss::Multicast_ResonanceFinished_Implementation(const bool bCompleted, const float HealAmount)
{
	if (!IsNetMode(NM_DedicatedServer))
	{
		BP_OnResonanceFinished(bCompleted, HealAmount);
	}
}

void AEMBColossusBoss::OnEncounterReset()
{
	// Rencontre abandonnée (wipe) : plus d'anneau en vol, plus de canalisation, noyau de nouveau protégé.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RingTimerHandle);
	}
	ActiveRings.Reset();
	if (bChannelingResonance)
	{
		InterruptResonance();
	}
	SetConditionalWeakPointsActive(false);
}
