// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBDragonBoss.h"

#include "EMBDragonAIController.h"
#include "EMBGameplayTags.h"

#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

AEMBDragonBoss::AEMBDragonBoss()
{
	AIControllerClass = AEMBDragonAIController::StaticClass();

	// PV, posture 1 500 et régénération 40/s après 4 s (section 6).
	ConfigureBossStats(EMBDragonDefaults::MaxHealthSolo, EMBDragonDefaults::MaxPoise,
		EMBDragonDefaults::PoiseRegenRate, EMBDragonDefaults::PoiseRegenDelay);

	// Phases : P2 à 70 % des PV, P3 à 35 % ; rugissement invulnérable de 5 s.
	PhaseHealthThresholds = { EMBDragonDefaults::Phase2HealthThreshold, EMBDragonDefaults::Phase3HealthThreshold };
	PhaseTransitionDuration = EMBDragonDefaults::PhaseTransitionDuration;
	StunDuration = EMBDragonDefaults::PostureBreakStunDuration;
	CriticalHitMultiplier = EMBDragonDefaults::CriticalHitMultiplier;
	CoopHealthBonusPerExtraPlayer = EMBDragonDefaults::CoopHealthBonusPerExtraPlayer;

	// Enragé en phase 3 : montages x1,15, recharges x0,8, télégraphes x0,9.
	EnrageFromPhase = 3;
	EnragedPlayRate = EMBDragonDefaults::EnragedPlayRate;
	EnragedCooldownMultiplier = EMBDragonDefaults::EnragedCooldownMultiplier;
	EnragedTelegraphMultiplier = EMBDragonDefaults::EnragedTelegraphMultiplier;

	VulnerableDamageMultiplier = EMBDragonDefaults::VulnerableDamageMultiplier;

	// Arène : caldera de Cendrehaute, rayon de rencontre 6 000 cm (maintien de la rencontre, abandon après un wipe).
	ArenaRadius = 6000.f;

	if (UCharacterMovementComponent* Move = GetCharacterMovement())
	{
		Move->MaxWalkSpeed = EMBDragonDefaults::WalkSpeed;
		Move->MaxFlySpeed = EMBDragonDefaults::FlySpeed;
	}

	// Points faibles par défaut (noms d'os à adapter au squelette final, table éditable dans le Blueprint).
	WeakPointMultipliers.Add(TEXT("head"), EMBDragonDefaults::WeakPointHead);
	WeakPointMultipliers.Add(TEXT("belly"), EMBDragonDefaults::WeakPointBelly);
	WeakPointMultipliers.Add(TEXT("wing_l"), EMBDragonDefaults::WeakPointWings);
	WeakPointMultipliers.Add(TEXT("wing_r"), EMBDragonDefaults::WeakPointWings);
	WeakPointMultipliers.Add(TEXT("tail_01"), EMBDragonDefaults::WeakPointTail);
}

FGameplayTag AEMBDragonBoss::GetDefeatedEventTag() const
{
	return EMBTags::Event_Boss_Killed_Dragon.GetTag();
}

float AEMBDragonBoss::GetAttackWeightModifier(const FEMBBossAttackRow& Row) const
{
	// Rafale d'ailes : réponse à un groupe de joueurs collés au dragon (au moins 2 à moins de 800 cm) : poids x3.
	if (Row.AttackTag == EMBTags::Attack_Dragon_WingBuffet.GetTag()
		&& CountPlayersWithin(EMBDragonDefaults::WingBuffetCrowdRadius) >= EMBDragonDefaults::WingBuffetCrowdCount)
	{
		return EMBDragonDefaults::WingBuffetCrowdMultiplier;
	}
	return 1.f;
}

void AEMBDragonBoss::OnPhaseEnteredServer(const int32 NewPhase, const int32 OldPhase)
{
	// Premier Cataclysm 20 s après l'entrée en phase 3.
	if (NewPhase >= 3 && GetWorld())
	{
		NextCataclysmTime = GetWorld()->GetTimeSeconds() + EMBDragonDefaults::CataclysmFirstDelayAfterPhase3;
	}
}

bool AEMBDragonBoss::IsCataclysmDue() const
{
	if (!HasAuthority() || IsDead() || IsTransitioning() || GetCurrentPhase() < 3 || NextCataclysmTime < 0.0)
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

void AEMBDragonBoss::OnEncounterBegun()
{
	// Première rencontre : phase 1, aucun effet. Reprise après un abandon en phase 3 : l'échéance du Cataclysm est
	// repoussée de 20 s à compter de la reprise (sinon elle serait immédiatement due).
	if (GetCurrentPhase() >= 3 && GetWorld())
	{
		NextCataclysmTime = GetWorld()->GetTimeSeconds() + EMBDragonDefaults::CataclysmFirstDelayAfterPhase3;
	}
}
