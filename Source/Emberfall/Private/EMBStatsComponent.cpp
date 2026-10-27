// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBStatsComponent.h"
#include "Emberfall.h"
#include "EMBCombatTypes.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

UEMBStatsComponent::UEMBStatsComponent()
{
	SetIsReplicatedByDefault(true);
	bWantsInitializeComponent = true;

	/*
	 * Choix d'optimisation : pas de Tick permanent.
	 * Le Tick ne sert qu'à la régénération (endurance, poise). Il démarre désactivé, n'est réveillé
	 * par WakeRegeneration() que lorsqu'une jauge quitte son maximum, et se rendort tout seul dès que
	 * tout est plein. Un acteur au repos (cas le plus fréquent, y compris le dragon hors combat)
	 * ne coûte donc aucun Tick. Quand il tourne, il le fait à 20 Hz (TickInterval) : la régénération
	 * utilise le DeltaTime réel, donc le résultat est identique à un tick par frame, pour 3 fois moins d'appels
	 * à 60 fps. Seul le serveur fait tourner la régénération.
	 */
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.TickInterval = 0.05f;

	MaxHealth = EMBCombatDefaults::MaxHealth;
	MaxStamina = EMBCombatDefaults::MaxStamina;
	StaminaRegenRate = EMBCombatDefaults::StaminaRegenRate;
	StaminaRegenDelay = EMBCombatDefaults::StaminaRegenDelay;
	ExhaustedRegenDelay = EMBCombatDefaults::ExhaustedRegenDelay;
	MaxPoise = EMBCombatDefaults::MaxPoise;
	PoiseRegenRate = EMBCombatDefaults::PoiseRegenRate;
	PoiseRegenDelay = EMBCombatDefaults::PoiseRegenDelay;

	Health = MaxHealth;
	Stamina = MaxStamina;
	Poise = GetMaxPoise();
	RepPoise = GetMaxPoise();
}

void UEMBStatsComponent::InitializeComponent()
{
	Super::InitializeComponent();

	// Initialisation côté serveur uniquement : les clients reçoivent les valeurs par réplication.
	// Fait ici (et non au BeginPlay) pour que les surcharges de MaxHealth faites en Blueprint ou par
	// SetMaxHealth() au BeginPlay du boss restent cohérentes quel que soit l'ordre des BeginPlay.
	if (HasAuthorityOwner())
	{
		Health = MaxHealth;
		Stamina = MaxStamina;
		Poise = GetMaxPoise();
		RepPoise = GetMaxPoise();
	}
}

float UEMBStatsComponent::GetPoise() const
{
	return HasAuthorityOwner() ? Poise : RepPoise;
}

void UEMBStatsComponent::SyncReplicatedPoise()
{
	// Seuil d'1 point : ne répliquer que les variations visibles, plus les bornes exactes (0 et maximum)
	// pour que l'UI n'affiche jamais une barre "presque pleine" alors qu'elle est pleine.
	if (FMath::Abs(Poise - RepPoise) >= 1.f || Poise <= 0.f || Poise >= GetMaxPoise())
	{
		RepPoise = Poise;
	}
}

void UEMBStatsComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthorityOwner())
	{
		SetComponentTickEnabled(false);
	}
}

bool UEMBStatsComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

float UEMBStatsComponent::GetMaxHealth() const
{
	return MaxHealth * HealthMaxMultiplier;
}

float UEMBStatsComponent::GetMaxStamina() const
{
	return MaxStamina * StaminaMaxMultiplier;
}

float UEMBStatsComponent::GetHealthPercent() const
{
	const float EffectiveMax = GetMaxHealth();
	return EffectiveMax > 0.f ? FMath::Clamp(Health / EffectiveMax, 0.f, 1.f) : 0.f;
}

void UEMBStatsComponent::BroadcastAttribute(EEMBStatType Type) const
{
	switch (Type)
	{
	case EEMBStatType::Health:
		OnAttributeChanged.Broadcast(Type, Health, GetMaxHealth());
		break;
	case EEMBStatType::Stamina:
		OnAttributeChanged.Broadcast(Type, Stamina, GetMaxStamina());
		break;
	case EEMBStatType::Poise:
		OnAttributeChanged.Broadcast(Type, GetPoise(), GetMaxPoise());
		break;
	}
}

void UEMBStatsComponent::WakeRegeneration()
{
	if (HasAuthorityOwner() && !IsComponentTickEnabled())
	{
		SetComponentTickEnabled(true);
	}
}

bool UEMBStatsComponent::ConsumeStamina(float Amount)
{
	if (!HasAuthorityOwner())
	{
		// Les clients ne modifient jamais les jauges : ils lisent la valeur répliquée.
		UE_LOG(LogEmberfall, Verbose, TEXT("ConsumeStamina ignoré sur un client (%s)."), *GetNameSafe(GetOwner()));
		return false;
	}

	if (IsDead() || Stamina <= 0.f)
	{
		return false;
	}
	if (!FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return true; // Rien à consommer, mais l'action reste permise.
	}

	Stamina = FMath::Max(0.f, Stamina - Amount);

	// Un joueur épuisé attend plus longtemps avant de récupérer (règle souls).
	const float Delay = (Stamina <= 0.f) ? ExhaustedRegenDelay : StaminaRegenDelay;
	StaminaRegenResumeTime = GetWorld()->GetTimeSeconds() + Delay;

	BroadcastAttribute(EEMBStatType::Stamina);
	WakeRegeneration();
	return true;
}

float UEMBStatsComponent::ApplyDamage(float Amount, AActor* DamageCauser)
{
	if (!HasAuthorityOwner() || IsDead() || !FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return 0.f;
	}

	const float Dealt = FMath::Min(Health, Amount);
	Health -= Dealt;
	BroadcastAttribute(EEMBStatType::Health);

	if (Health <= 0.f)
	{
		Health = 0.f;
		// Plus de régénération pour un mort.
		SetComponentTickEnabled(false);
		OnDeath.Broadcast(DamageCauser);
	}
	return Dealt;
}

bool UEMBStatsComponent::ApplyPoiseDamage(float Amount, AActor* Instigator)
{
	if (!HasAuthorityOwner() || IsDead() || !FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return false;
	}

	Poise -= Amount;
	// Tout dégât de poise (même sans rupture) repousse la régénération.
	PoiseRegenResumeTime = GetWorld()->GetTimeSeconds() + PoiseRegenDelay;

	if (Poise <= 0.f)
	{
		// Rupture : la jauge est remise au maximum (le coup suivant ne brise pas immédiatement).
		Poise = GetMaxPoise();
		SyncReplicatedPoise();
		BroadcastAttribute(EEMBStatType::Poise);
		OnPoiseBroken.Broadcast(Instigator);
		return true;
	}

	SyncReplicatedPoise();
	BroadcastAttribute(EEMBStatType::Poise);
	WakeRegeneration();
	return false;
}

void UEMBStatsComponent::Heal(float Amount)
{
	if (!HasAuthorityOwner() || IsDead() || !FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return;
	}
	Health = FMath::Min(GetMaxHealth(), Health + Amount);
	BroadcastAttribute(EEMBStatType::Health);
}

void UEMBStatsComponent::RestoreFromSave(float InHealth, float InStamina)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// Valeurs non numériques ou aberrantes (sauvegarde corrompue) : on retombe sur le plein.
	Health = FMath::IsFinite(InHealth) ? FMath::Clamp(InHealth, 1.f, GetMaxHealth()) : GetMaxHealth();
	Stamina = FMath::IsFinite(InStamina) ? FMath::Clamp(InStamina, 0.f, GetMaxStamina()) : GetMaxStamina();
	Poise = GetMaxPoise();
	SyncReplicatedPoise();

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	StaminaRegenResumeTime = Now;
	PoiseRegenResumeTime = Now;

	BroadcastAttribute(EEMBStatType::Health);
	BroadcastAttribute(EEMBStatType::Stamina);
	BroadcastAttribute(EEMBStatType::Poise);
	WakeRegeneration();
}

void UEMBStatsComponent::SetStaminaRegenMultiplier(float Multiplier)
{
	StaminaRegenMultiplier = FMath::Max(0.f, Multiplier);
}

void UEMBStatsComponent::SetMaxHealth(float NewMax, bool bRefill)
{
	if (!HasAuthorityOwner() || !FMath::IsFinite(NewMax))
	{
		return;
	}
	MaxHealth = FMath::Max(1.f, NewMax);
	Health = bRefill ? GetMaxHealth() : FMath::Min(Health, GetMaxHealth());
	BroadcastAttribute(EEMBStatType::Health);
}

void UEMBStatsComponent::SetSurvivalModifiers(float MaxHealthMultiplier, float MaxStaminaMultiplier, float StaminaRegenMultiplierFromSurvival)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// Garde-fous : un multiplicateur nul ou négatif rendrait la jauge inutilisable ; on plafonne aussi le haut
	// pour qu'une valeur aberrante ne crée pas de vie démesurée.
	const float NewHealthMult = FMath::IsFinite(MaxHealthMultiplier) ? FMath::Clamp(MaxHealthMultiplier, 0.05f, 4.f) : 1.f;
	const float NewStaminaMult = FMath::IsFinite(MaxStaminaMultiplier) ? FMath::Clamp(MaxStaminaMultiplier, 0.05f, 4.f) : 1.f;
	const float NewRegenMult = FMath::IsFinite(StaminaRegenMultiplierFromSurvival) ? FMath::Max(0.f, StaminaRegenMultiplierFromSurvival) : 1.f;

	const bool bChanged = !FMath::IsNearlyEqual(NewHealthMult, HealthMaxMultiplier)
		|| !FMath::IsNearlyEqual(NewStaminaMult, StaminaMaxMultiplier);

	HealthMaxMultiplier = NewHealthMult;
	StaminaMaxMultiplier = NewStaminaMult;
	SurvivalStaminaRegenMultiplier = NewRegenMult;

	if (bChanged)
	{
		// Les valeurs courantes sont plafonnées au nouveau maximum, jamais relevées (pas de soin gratuit).
		Health = FMath::Min(Health, GetMaxHealth());
		Stamina = FMath::Min(Stamina, GetMaxStamina());
		BroadcastAttribute(EEMBStatType::Health);
		BroadcastAttribute(EEMBStatType::Stamina);
		WakeRegeneration(); // Un maximum relevé laisse l'endurance sous son nouveau plafond.
	}
}

void UEMBStatsComponent::SetEquipmentPoiseBonus(float Bonus)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	const float NewBonus = FMath::IsFinite(Bonus) ? FMath::Max(0.f, Bonus) : 0.f;
	if (FMath::IsNearlyEqual(NewBonus, EquipmentPoiseBonus))
	{
		return;
	}

	EquipmentPoiseBonus = NewBonus;
	Poise = FMath::Min(Poise, GetMaxPoise());
	SyncReplicatedPoise();
	BroadcastAttribute(EEMBStatType::Poise);
	WakeRegeneration(); // Un maximum relevé laisse la poise sous son nouveau plafond : elle se régénère.
}

void UEMBStatsComponent::ReviveWithFraction(float HealthFraction, float StaminaFraction)
{
	if (!HasAuthorityOwner() || !IsDead())
	{
		return;
	}

	const float HealthRatio = FMath::IsFinite(HealthFraction) ? FMath::Clamp(HealthFraction, 0.f, 1.f) : 1.f;
	const float StaminaRatio = FMath::IsFinite(StaminaFraction) ? FMath::Clamp(StaminaFraction, 0.f, 1.f) : 1.f;

	Health = FMath::Max(1.f, GetMaxHealth() * HealthRatio); // Jamais 0 : ce serait rester mort.
	Stamina = GetMaxStamina() * StaminaRatio;
	Poise = GetMaxPoise();
	SyncReplicatedPoise();

	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	StaminaRegenResumeTime = Now + StaminaRegenDelay;
	PoiseRegenResumeTime = Now;

	BroadcastAttribute(EEMBStatType::Health);
	BroadcastAttribute(EEMBStatType::Stamina);
	BroadcastAttribute(EEMBStatType::Poise);
	WakeRegeneration(); // ApplyDamage avait éteint le Tick à la mort.
}

void UEMBStatsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!HasAuthorityOwner() || IsDead())
	{
		SetComponentTickEnabled(false);
		return;
	}

	const double Now = GetWorld()->GetTimeSeconds();
	bool bNeedsMoreTicks = false;

	const float EffectiveMaxStamina = GetMaxStamina();
	if (Stamina < EffectiveMaxStamina)
	{
		bNeedsMoreTicks = true;
		if (Now >= StaminaRegenResumeTime)
		{
			// Multiplicateurs combinés : garde (x0,35) et survie (faim, chaleur).
			Stamina = FMath::Min(EffectiveMaxStamina, Stamina + StaminaRegenRate * StaminaRegenMultiplier * SurvivalStaminaRegenMultiplier * DeltaTime);
			BroadcastAttribute(EEMBStatType::Stamina);
		}
	}

	if (Poise < GetMaxPoise())
	{
		bNeedsMoreTicks = true;
		if (Now >= PoiseRegenResumeTime)
		{
			Poise = FMath::Min(GetMaxPoise(), Poise + PoiseRegenRate * DeltaTime);
			SyncReplicatedPoise();
			BroadcastAttribute(EEMBStatType::Poise);
		}
	}

	// Tout est plein : on s'endort jusqu'à la prochaine consommation (voir le commentaire du constructeur).
	if (!bNeedsMoreTicks)
	{
		SetComponentTickEnabled(false);
	}
}

void UEMBStatsComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UEMBStatsComponent, Health);
	DOREPLIFETIME(UEMBStatsComponent, RepPoise);

	// L'endurance change en continu (régénération) : la répliquer à tous les clients serait du
	// trafic gaspillé, seul le propriétaire (HUD) en a besoin.
	DOREPLIFETIME_CONDITION(UEMBStatsComponent, Stamina, COND_OwnerOnly);

	DOREPLIFETIME(UEMBStatsComponent, MaxHealth);

	// Multiplicateurs de survie : la vie max se voit de tous (barre de groupe), l'endurance max du propriétaire seul.
	DOREPLIFETIME(UEMBStatsComponent, HealthMaxMultiplier);
	DOREPLIFETIME_CONDITION(UEMBStatsComponent, StaminaMaxMultiplier, COND_OwnerOnly);
	DOREPLIFETIME(UEMBStatsComponent, EquipmentPoiseBonus);

	// Maxima d'endurance et de poise fixés par les valeurs par défaut : envoyés une seule fois.
	DOREPLIFETIME_CONDITION(UEMBStatsComponent, MaxStamina, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(UEMBStatsComponent, MaxPoise, COND_InitialOnly);
}

void UEMBStatsComponent::OnRep_Health(float OldValue)
{
	BroadcastAttribute(EEMBStatType::Health);
}

void UEMBStatsComponent::OnRep_Stamina(float OldValue)
{
	BroadcastAttribute(EEMBStatType::Stamina);
}

void UEMBStatsComponent::OnRep_Poise(float OldValue)
{
	BroadcastAttribute(EEMBStatType::Poise);
}

void UEMBStatsComponent::OnRep_MaxHealth(float OldValue)
{
	BroadcastAttribute(EEMBStatType::Health);
}

void UEMBStatsComponent::OnRep_MaxMultipliers()
{
	BroadcastAttribute(EEMBStatType::Health);
	BroadcastAttribute(EEMBStatType::Stamina);
	BroadcastAttribute(EEMBStatType::Poise);
}
