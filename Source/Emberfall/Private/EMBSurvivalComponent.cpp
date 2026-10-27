// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBSurvivalComponent.h"
#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBGameplayTags.h"
#include "EMBInventoryComponent.h"
#include "EMBStatsComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Palier public de Corruption : 0 (< 50), 1 (>= 50), 2 (>= 75). */
	uint8 SurvivalCorruptionTier(float Corruption)
	{
		if (Corruption >= EMBSurvivalDefaults::CriticalThreshold)
		{
			return 2;
		}
		return Corruption >= EMBSurvivalDefaults::MajorThreshold ? 1 : 0;
	}

	/** Remplace une valeur non finie (NaN, infini) par 0 : une ligne de table ou un RPC ne doit pas empoisonner les jauges. */
	float SurvivalFiniteOrZero(float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}

	/** Borne une valeur lue (sauvegarde) ; une valeur non finie retombe sur Fallback. */
	float SurvivalSanitizeRange(float Value, float Fallback, float Min, float Max)
	{
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, Min, Max) : Fallback;
	}
}

UEMBSurvivalComponent::UEMBSurvivalComponent()
{
	// Pas de Tick : la logique tourne sur un timer serveur à 1 Hz.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);

	ConsumablesTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_Consumables.DT_Consumables")));
}

// ---------------------------------------------------------------- Cycle de vie

void UEMBSurvivalComponent::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	if (!World || !HasAuthorityOwner())
	{
		return; // Les clients ne simulent rien : ils reçoivent les jauges répliquées.
	}

	ResolveDependencies();

	if (!ConsumablesTable.IsNull())
	{
		LoadedConsumables = ConsumablesTable.LoadSynchronous();
		if (!LoadedConsumables)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBSurvivalComponent: DT_Consumables introuvable (%s) : aucune consommation possible."), *ConsumablesTable.ToString());
		}
	}

	LastTickTime = World->GetTimeSeconds();

	// Premier déclenchement décalé aléatoirement : les composants de plusieurs joueurs ne tombent pas tous sur la même frame.
	const float FirstDelay = EMBSurvivalDefaults::TickInterval * FMath::FRandRange(0.5f, 1.5f);
	World->GetTimerManager().SetTimer(SurvivalTimerHandle, this, &UEMBSurvivalComponent::SurvivalTick, EMBSurvivalDefaults::TickInterval, true, FirstDelay);

	PushEffects(true);
	RefreshReplicatedState();
}

void UEMBSurvivalComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SurvivalTimerHandle);
	}
	if (Combat)
	{
		Combat->OnHitReceived.RemoveDynamic(this, &UEMBSurvivalComponent::HandleHitReceived);
	}

	Super::EndPlay(EndPlayReason);
}

void UEMBSurvivalComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Données de HUD qui bougent chaque seconde : propriétaire seul.
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, Hunger, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, Thirst, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, FeltTemperature, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, Corruption, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, bIntoxicated, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, bFireOilActive, COND_OwnerOnly);

	// Corruption visible des autres joueurs : palier seulement (voir l'en-tête), jamais renvoyé au propriétaire.
	DOREPLIFETIME_CONDITION(UEMBSurvivalComponent, PublicCorruptionTier, COND_SkipOwner);
}

bool UEMBSurvivalComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

void UEMBSurvivalComponent::ResolveDependencies()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// Recherche par classe : le composant fonctionne qu'il soit créé en C++ ou ajouté par Blueprint.
	if (!Stats)
	{
		Stats = Owner->FindComponentByClass<UEMBStatsComponent>();
	}
	if (!Combat)
	{
		Combat = Owner->FindComponentByClass<UEMBCombatComponent>();
	}
	if (!Inventory)
	{
		Inventory = Owner->FindComponentByClass<UEMBInventoryComponent>();
	}

	// Corruption des coups reçus : serveur seulement (le multicast de combat s'exécute aussi chez les clients).
	if (Combat && HasAuthorityOwner())
	{
		Combat->OnHitReceived.AddUniqueDynamic(this, &UEMBSurvivalComponent::HandleHitReceived);
	}
}

UEMBEnvironmentSubsystem* UEMBSurvivalComponent::GetEnvironment() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr;
}

// ---------------------------------------------------------------- Lecture

bool UEMBSurvivalComponent::HasWhispers() const
{
	return Corruption >= EMBSurvivalDefaults::WhispersThreshold;
}

bool UEMBSurvivalComponent::HasSurvivalTag(FGameplayTag Tag) const
{
	return Tag.IsValid() && Tag.MatchesTagExact(EMBTags::State_Survival_Whispers.GetTag()) && HasWhispers();
}

uint8 UEMBSurvivalComponent::GetVisibleCorruptionTier() const
{
	// Un proxy simulé (personnage d'un autre joueur) ne reçoit que le palier ; les autres rôles ont la valeur exacte.
	return GetOwnerRole() == ROLE_SimulatedProxy ? PublicCorruptionTier : SurvivalCorruptionTier(Corruption);
}

float UEMBSurvivalComponent::GetComfortRange() const
{
	return FMath::Clamp(PassiveModifiers.ComfortRange, 0.f, EMBSurvivalDefaults::MaxComfortRangeC);
}

bool UEMBSurvivalComponent::IsColdForHunger() const
{
	return FeltTemperature < EMBSurvivalDefaults::ColdHungerThresholdC - GetComfortRange();
}

bool UEMBSurvivalComponent::IsHotForThirst() const
{
	return FeltTemperature > EMBSurvivalDefaults::HotThirstThresholdC + GetComfortRange();
}

// ---------------------------------------------------------------- Boucle serveur (1 Hz)

void UEMBSurvivalComponent::SurvivalTick()
{
	using namespace EMBSurvivalDefaults;

	UWorld* World = GetWorld();
	AActor* Owner = GetOwner();
	if (!World || !IsValid(Owner) || !HasAuthorityOwner())
	{
		return;
	}

	// Durée réellement écoulée (temps monde), plafonnée : un timer retardé ne doit pas vider les jauges d'un coup.
	const double Now = World->GetTimeSeconds();
	const float DeltaSeconds = FMath::Clamp(static_cast<float>(Now - LastTickTime), 0.f, MaxTickDelta);
	LastTickTime = Now;

	ResolveDependencies();
	if (DeltaSeconds <= 0.f || (Stats && Stats->IsDead()))
	{
		return; // Mort : les jauges sont figées jusqu'à ApplyRespawnValues.
	}

	// ---- Lecture de l'environnement (valeurs neutres si le sous-système est absent)
	const UEMBEnvironmentSubsystem* Environment = GetEnvironment();
	const FVector Location = Owner->GetActorLocation();
	const float AmbientC = Environment ? Environment->GetAmbientTemperatureAt(Location) : DefaultAmbientTemperatureC;
	const float HeatBonusC = Environment ? Environment->GetHeatBonusAt(Location) : 0.f;
	const float ZoneCorruptionRate = Environment ? Environment->GetCorruptionRateAt(Location) : DefaultCorruptionRatePerSecond;
	const float PurificationRate = Environment ? Environment->GetPurificationRateAt(Location) : 0.f;

	// ---- Température ressentie : cible = ambiante + isolation + chaleur du feu, atteinte à 1 °C/s
	const float TargetC = FMath::Clamp(AmbientC + InsulationC + HeatBonusC, MinFeltTemperatureC, MaxFeltTemperatureC);
	FeltTemperature = FMath::FInterpConstantTo(FeltTemperature, TargetC, DeltaSeconds, TemperatureSmoothingPerSecond);

	// ---- Faim et soif (le passif HungerThirstRate est un bonus additif : multiplicateur = max(0, 1 + somme))
	const float RateMultiplier = FMath::Max(0.f, 1.f + PassiveModifiers.HungerThirstRate);
	const float HungerPerSecond = HungerLossPerMinute / 60.f * (IsColdForHunger() ? ColdHungerMultiplier : 1.f) * RateMultiplier;
	const float ThirstPerSecond = ThirstLossPerMinute / 60.f * (IsHotForThirst() ? HotThirstMultiplier : 1.f) * RateMultiplier;
	Hunger = FMath::Max(0.f, Hunger - HungerPerSecond * DeltaSeconds);
	Thirst = FMath::Max(0.f, Thirst - ThirstPerSecond * DeltaSeconds);

	// ---- Corruption : zone (déjà ×2 la nuit côté environnement) × passif, moins la purification
	const float CorruptionGainMultiplier = FMath::Max(0.f, 1.f + PassiveModifiers.CorruptionGain);
	Corruption = FMath::Clamp(Corruption + (ZoneCorruptionRate * CorruptionGainMultiplier - PurificationRate) * DeltaSeconds, 0.f, MaxCorruption);

	// ---- Dégâts de survie du tick, regroupés en une seule frappe
	float Damage = 0.f;
	if (Hunger <= KINDA_SMALL_NUMBER)
	{
		Damage += StarvationDamagePerSecond * DeltaSeconds;
	}
	if (Thirst <= KINDA_SMALL_NUMBER)
	{
		Damage += DehydrationDamagePerSecond * DeltaSeconds;
	}
	// Seuils extrêmes décalés par ComfortRange comme les autres.
	if (FeltTemperature < FreezingThresholdC - GetComfortRange())
	{
		Damage += FreezingDamagePerSecond * DeltaSeconds;
	}
	if (FeltTemperature > HeatstrokeThresholdC + GetComfortRange())
	{
		Damage += HeatstrokeDamagePerSecond * DeltaSeconds;
	}
	if (IntoxicationRemaining > 0.f)
	{
		// Si l'intoxication s'achève au milieu du tick, seule la fraction restante fait mal.
		Damage += IntoxicationDamagePerSecond * FMath::Min(DeltaSeconds, IntoxicationRemaining);
		IntoxicationRemaining = FMath::Max(0.f, IntoxicationRemaining - DeltaSeconds);
	}
	if (FireOilRemaining > 0.f)
	{
		FireOilRemaining = FMath::Max(0.f, FireOilRemaining - DeltaSeconds);
	}

	if (Damage > 0.f && ApplySurvivalDamage(Damage))
	{
		RefreshReplicatedState();
		return; // Mort de survie : plus rien à évaluer.
	}

	// Effets (seuils de jauges, huile expirée), mort à 100 de Corruption, état répliqué, interface.
	OnCorruptionChanged();
}

bool UEMBSurvivalComponent::ApplySurvivalDamage(float Amount)
{
	AActor* Owner = GetOwner();
	if (!Owner || !(Amount > 0.f))
	{
		return Stats && Stats->IsDead();
	}

	// CHOIX : ApplyHitToActor plutôt que Stats->ApplyDamage direct. Un coup en bDamageOverTime est un dégât
	// direct (ni esquive, ni garde, ni parade, ni multicast, et OnHitReceived n'est pas diffusé : pas de
	// boucle de Corruption), mais le routage reste centralisé dans le combat (état Dead ignoré, futures
	// résistances) et se replie sur les stats si le propriétaire n'a pas de composant de combat.
	// Instigator = nullptr : pas de tueur (la mort de faim n'est créditée à personne) et le combat n'ignore
	// que les coups dont l'instigateur est le propriétaire lui-même.
	FEMBHitData Hit;
	Hit.Damage = Amount;
	Hit.DamageType = EEMBDamageType::Physical;
	Hit.bDamageOverTime = true;
	Hit.bUnblockable = true;
	Hit.bUnparryable = true;
	Hit.HitLocation = Owner->GetActorLocation();

	const EEMBHitResult Result = UEMBCombatComponent::ApplyHitToActor(Owner, Hit);
	return Result == EEMBHitResult::Killed || (Stats && Stats->IsDead());
}

void UEMBSurvivalComponent::KillByCorruption()
{
	if (!Stats || Stats->IsDead())
	{
		return;
	}
	// Mort normale (9.2 : l'ennemi « Possédé » n'est pas codé). Dégâts = PV courants + 1, par le même chemin
	// que les autres dégâts de survie, donc sans esquive possible ni invulnérabilité de réapparition.
	ApplySurvivalDamage(Stats->GetHealth() + 1.f);
}

// ---------------------------------------------------------------- Effets poussés

UEMBSurvivalComponent::FEffects UEMBSurvivalComponent::ComputeEffects() const
{
	using namespace EMBSurvivalDefaults;

	FEffects Effects;

	// Corruption : PV max.
	if (Corruption >= CriticalThreshold)
	{
		Effects.MaxHealthMultiplier = CriticalMaxHealthMultiplier;
	}
	else if (Corruption >= MajorThreshold)
	{
		Effects.MaxHealthMultiplier = MajorMaxHealthMultiplier;
	}

	// Soif basse : endurance max. Faim basse et chaleur : régénération (les malus se multiplient).
	if (Thirst < LowGaugeThreshold)
	{
		Effects.MaxStaminaMultiplier = LowThirstMaxStaminaMultiplier;
	}
	if (Hunger < LowGaugeThreshold)
	{
		Effects.StaminaRegenMultiplier *= LowHungerStaminaRegenMultiplier;
	}
	if (IsHotForThirst())
	{
		Effects.StaminaRegenMultiplier *= HotStaminaRegenMultiplier;
	}

	// Huile ignifuge : bonus temporaire seul (le passif FireResistance est poussé par la progression).
	if (FireOilRemaining > 0.f)
	{
		Effects.ExtraFireResistance = FMath::Clamp(FireOilBonus, 0.f, MaxFireResistanceBonus);
	}
	return Effects;
}

void UEMBSurvivalComponent::PushEffects(bool bForce)
{
	const FEffects Effects = ComputeEffects();

	// Poussé seulement quand la valeur change (ou à la demande : nouvel état, nouveau pawn).
	if (Stats)
	{
		const bool bChanged = bForce || !bStatsEffectsApplied
			|| !FMath::IsNearlyEqual(Effects.MaxHealthMultiplier, AppliedEffects.MaxHealthMultiplier)
			|| !FMath::IsNearlyEqual(Effects.MaxStaminaMultiplier, AppliedEffects.MaxStaminaMultiplier)
			|| !FMath::IsNearlyEqual(Effects.StaminaRegenMultiplier, AppliedEffects.StaminaRegenMultiplier);
		if (bChanged)
		{
			Stats->SetSurvivalModifiers(Effects.MaxHealthMultiplier, Effects.MaxStaminaMultiplier, Effects.StaminaRegenMultiplier);
			AppliedEffects.MaxHealthMultiplier = Effects.MaxHealthMultiplier;
			AppliedEffects.MaxStaminaMultiplier = Effects.MaxStaminaMultiplier;
			AppliedEffects.StaminaRegenMultiplier = Effects.StaminaRegenMultiplier;
			bStatsEffectsApplied = true;
		}
	}

	if (Combat)
	{
		const bool bChanged = bForce || !bFireEffectApplied
			|| !FMath::IsNearlyEqual(Effects.ExtraFireResistance, AppliedEffects.ExtraFireResistance);
		if (bChanged)
		{
			Combat->SetExtraFireResistance(Effects.ExtraFireResistance);
			AppliedEffects.ExtraFireResistance = Effects.ExtraFireResistance;
			bFireEffectApplied = true;
		}
	}
}

void UEMBSurvivalComponent::OnCorruptionChanged()
{
	if (Corruption >= EMBSurvivalDefaults::MaxCorruption)
	{
		KillByCorruption();
	}
	PushEffects(false);
	RefreshReplicatedState();
}

void UEMBSurvivalComponent::RefreshReplicatedState()
{
	// Réaffecter une valeur identique ne génère aucun trafic : seules les vraies transitions sont envoyées.
	PublicCorruptionTier = SurvivalCorruptionTier(Corruption);
	bIntoxicated = IntoxicationRemaining > 0.f;
	bFireOilActive = FireOilRemaining > 0.f;
	BroadcastSurvival();
}

void UEMBSurvivalComponent::BroadcastSurvival()
{
	OnSurvivalChanged.Broadcast(Hunger, Thirst, FeltTemperature, Corruption);
}

void UEMBSurvivalComponent::OnRep_Gauges()
{
	BroadcastSurvival();
}

// ---------------------------------------------------------------- Corruption

void UEMBSurvivalComponent::HandleHitReceived(const FEMBHitData& Hit, EEMBHitResult Result)
{
	using namespace EMBSurvivalDefaults;

	if (!HasAuthorityOwner())
	{
		return;
	}

	// Un coup esquivé, paré ou ignoré ne corrompt pas ; une garde ne retient qu'une partie de la Corruption.
	float Factor = 1.f;
	switch (Result)
	{
	case EEMBHitResult::Hit:
	case EEMBHitResult::GuardBroken:
	case EEMBHitResult::Killed:
		break;
	case EEMBHitResult::Blocked:
		Factor = BlockedCorruptionFactor;
		break;
	default:
		return;
	}

	float Amount = SurvivalFiniteOrZero(Hit.CorruptionAmount);
	if (Amount <= 0.f && Hit.DamageType == EEMBDamageType::Corruption)
	{
		// Pas de montant explicite : clamp(0,2 x dégâts, 2, 20).
		Amount = FMath::Clamp(HitCorruptionFactor * SurvivalFiniteOrZero(Hit.Damage), HitCorruptionMin, HitCorruptionMax);
	}
	if (Amount > 0.f)
	{
		AddCorruption(Amount * Factor);
	}
}

void UEMBSurvivalComponent::AddCorruption(float Amount)
{
	if (!HasAuthorityOwner() || !FMath::IsFinite(Amount) || Amount <= 0.f || (Stats && Stats->IsDead()))
	{
		return;
	}

	// Le passif CorruptionGain (bonus additif, ex. -0,15) s'applique à tout gain.
	const float Multiplier = FMath::Max(0.f, 1.f + PassiveModifiers.CorruptionGain);
	Corruption = FMath::Clamp(Corruption + Amount * Multiplier, 0.f, EMBSurvivalDefaults::MaxCorruption);
	OnCorruptionChanged();
}

void UEMBSurvivalComponent::ReduceCorruption(float Amount)
{
	if (!HasAuthorityOwner() || !FMath::IsFinite(Amount) || Amount <= 0.f)
	{
		return;
	}

	Corruption = FMath::Clamp(Corruption - Amount, 0.f, EMBSurvivalDefaults::MaxCorruption);
	OnCorruptionChanged();
}

// ---------------------------------------------------------------- Réglages

void UEMBSurvivalComponent::ApplyRespawnValues()
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	Hunger = EMBSurvivalDefaults::RespawnHunger;
	Thirst = EMBSurvivalDefaults::RespawnThirst;
	Corruption = 0.f;
	FeltTemperature = EMBSurvivalDefaults::NeutralTemperatureC;
	IntoxicationRemaining = 0.f;
	FireOilRemaining = 0.f;
	LastConsumeTime = -1000.0;

	if (const UWorld* World = GetWorld())
	{
		LastTickTime = World->GetTimeSeconds();
	}

	ResolveDependencies();
	PushEffects(true); // Le pawn peut être neuf : on repousse les effets (PV max, endurance) même sans changement.
	RefreshReplicatedState();
}

void UEMBSurvivalComponent::ApplyReviveValues()
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// Réanimation : on ne repart ni à 75+ (PV max -30 %) ni à 100 ; l'intoxication est purgée, le reste est conservé.
	Corruption = FMath::Min(Corruption, EMBSurvivalDefaults::ReviveCorruptionCap);
	Hunger = FMath::Max(Hunger, EMBSurvivalDefaults::ReviveMinHungerThirst);
	Thirst = FMath::Max(Thirst, EMBSurvivalDefaults::ReviveMinHungerThirst);
	IntoxicationRemaining = 0.f;

	ResolveDependencies();
	OnCorruptionChanged(); // Effets (PV max), états répliqués et interface.
}

void UEMBSurvivalComponent::SetPassiveModifiers(const FEMBPassiveModifiers& Mods)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// Seuls les trois passifs de survie sont retenus (FireResistance : voir l'en-tête). Valeurs non finies neutralisées.
	PassiveModifiers.HungerThirstRate = SurvivalFiniteOrZero(Mods.HungerThirstRate);
	PassiveModifiers.ComfortRange = SurvivalFiniteOrZero(Mods.ComfortRange);
	PassiveModifiers.CorruptionGain = SurvivalFiniteOrZero(Mods.CorruptionGain);

	// ComfortRange décale les seuils : les effets peuvent changer sans attendre le prochain tick.
	PushEffects(false);
	RefreshReplicatedState();
}

void UEMBSurvivalComponent::SetInsulation(float DegreesC)
{
	if (!HasAuthorityOwner() || !FMath::IsFinite(DegreesC))
	{
		return;
	}
	InsulationC = FMath::Clamp(DegreesC, -EMBSurvivalDefaults::MaxInsulationC, EMBSurvivalDefaults::MaxInsulationC);
}

// ---------------------------------------------------------------- Sauvegarde

void UEMBSurvivalComponent::ExportSaveData(FEMBSurvivalSaveData& Out) const
{
	Out.Hunger = Hunger;
	Out.Thirst = Thirst;
	Out.Corruption = Corruption;
	Out.FeltTemperature = FeltTemperature;
	Out.FireOilRemaining = FireOilRemaining;
	Out.IntoxicationRemaining = IntoxicationRemaining;
}

void UEMBSurvivalComponent::ImportSaveData(const FEMBSurvivalSaveData& In)
{
	using namespace EMBSurvivalDefaults;

	if (!HasAuthorityOwner())
	{
		return;
	}

	// Une sauvegarde est une entrée non fiable (fichier édité, corrompu) : tout est borné.
	// Les défauts de FEMBSurvivalSaveData valent 0 : une structure neuve donnerait faim 0 et soif 0. La sauvegarde v2
	// n'appelle cet import que si l'état existe (bHasSurvivalState) ; pour un joueur neuf, ne pas importer
	// (valeurs de départ du composant) ou appeler ApplyRespawnValues.
	Hunger = SurvivalSanitizeRange(In.Hunger, StartHunger, 0.f, MaxGauge);
	Thirst = SurvivalSanitizeRange(In.Thirst, StartThirst, 0.f, MaxGauge);
	// Jamais 100 : le joueur serait tué au premier tick ; une sauvegarde saine ne contient pas un joueur mort.
	Corruption = SurvivalSanitizeRange(In.Corruption, 0.f, 0.f, MaxImportedCorruption);
	FeltTemperature = SurvivalSanitizeRange(In.FeltTemperature, NeutralTemperatureC, MinFeltTemperatureC, MaxFeltTemperatureC);

	IntoxicationRemaining = SurvivalSanitizeRange(In.IntoxicationRemaining, 0.f, 0.f, MaxIntoxicationSeconds);
	FireOilRemaining = SurvivalSanitizeRange(In.FireOilRemaining, 0.f, 0.f, MaxFireOilSeconds);
	// Intensités non sauvegardées (contrat 10.4) : valeurs de référence du référentiel.
	IntoxicationDamagePerSecond = DefaultIntoxicationDamagePerSecond;
	FireOilBonus = DefaultFireOilBonus;

	if (const UWorld* World = GetWorld())
	{
		LastTickTime = World->GetTimeSeconds();
	}

	ResolveDependencies();
	PushEffects(true);
	RefreshReplicatedState();
}

// ---------------------------------------------------------------- Consommables

void UEMBSurvivalComponent::RequestConsume(FName ConsumableId)
{
	if (ConsumableId.IsNone())
	{
		return;
	}

	if (HasAuthorityOwner())
	{
		ProcessConsume(ConsumableId); // Hôte d'une partie en écoute, ou appel serveur : même validation.
		return;
	}

	// Client : filtre local léger (le serveur reste l'arbitre) pour ne pas envoyer de RPC voués au refus.
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now - LastLocalConsumeRequestTime < EMBSurvivalDefaults::ConsumeDelay - EMBSurvivalDefaults::ConsumeDelayTolerance)
	{
		return;
	}
	LastLocalConsumeRequestTime = Now;
	Server_Consume(ConsumableId);
}

void UEMBSurvivalComponent::Server_Consume_Implementation(FName ConsumableId)
{
	// Pas de _Validate : un argument invalide est refusé sans déconnecter le joueur (ProcessConsume valide tout).
	ProcessConsume(ConsumableId);
}

const FEMBConsumableRow* UEMBSurvivalComponent::FindConsumableRow(FName ConsumableId) const
{
	if (ConsumableId.IsNone() || !LoadedConsumables)
	{
		return nullptr;
	}
	// bWarnIfRowMissing = false : un identifiant inconnu venant du réseau ne doit pas inonder le journal.
	return LoadedConsumables->FindRow<FEMBConsumableRow>(ConsumableId, TEXT("UEMBSurvivalComponent::FindConsumableRow"), false);
}

bool UEMBSurvivalComponent::WouldHaveEffect(const FEMBConsumableRow& Row) const
{
	using namespace EMBSurvivalDefaults;

	// Un consommable sans aucun effet utile n'est pas consommé (l'unité n'est pas gaspillée).
	// Une variation négative (nourriture avariée, par conception) compte comme un effet.
	if (Row.HungerDelta < 0.f || (Row.HungerDelta > 0.f && Hunger < MaxGauge))
	{
		return true;
	}
	if (Row.ThirstDelta < 0.f || (Row.ThirstDelta > 0.f && Thirst < MaxGauge))
	{
		return true;
	}
	if (Row.HealthPercent > 0.f && Stats && Stats->GetHealth() < Stats->GetMaxHealth())
	{
		return true;
	}
	if (Row.CorruptionDelta > 0.f || (Row.CorruptionDelta < 0.f && Corruption > 0.f))
	{
		return true;
	}
	if (Row.bCuresIntoxication && IntoxicationRemaining > 0.f)
	{
		return true;
	}
	if (Row.FireResistanceBonus > 0.f && Row.FireResistanceDuration > FireOilRemaining)
	{
		return true;
	}
	return false;
}

void UEMBSurvivalComponent::ProcessConsume(FName ConsumableId)
{
	using namespace EMBSurvivalDefaults;

	const UWorld* World = GetWorld();
	if (!World || !HasAuthorityOwner() || ConsumableId.IsNone())
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	// 1) Plafond de requêtes : 5 par seconde (fenêtre fixe), toutes comptées, y compris les refusées.
	if (Now - ConsumeWindowStart >= 1.0)
	{
		ConsumeWindowStart = Now;
		ConsumeRequestsInWindow = 0;
	}
	if (++ConsumeRequestsInWindow > MaxConsumeRequestsPerSecond)
	{
		return;
	}

	// 2) Délai de 1 s entre deux consommations réussies (tolérance de gigue réseau).
	if (Now - LastConsumeTime < ConsumeDelay - ConsumeDelayTolerance)
	{
		return;
	}

	// 3) État du personnage.
	ResolveDependencies();
	if (!Stats || Stats->IsDead() || !Inventory)
	{
		return;
	}
	if (Combat && Combat->GetCurrentState() == EEMBCombatState::Dead)
	{
		return;
	}

	// 4) Identifiant connu de la table (chargement tardif si le BeginPlay n'a pas pu la lire).
	if (!LoadedConsumables && !ConsumablesTable.IsNull())
	{
		LoadedConsumables = ConsumablesTable.LoadSynchronous();
	}
	const FEMBConsumableRow* Row = FindConsumableRow(ConsumableId);
	if (!Row)
	{
		UE_LOG(LogEmberfall, Verbose, TEXT("UEMBSurvivalComponent: consommable '%s' inconnu, requete ignoree."), *ConsumableId.ToString());
		return;
	}

	// 5) Le joueur possède l'unité et elle servirait à quelque chose.
	if (Inventory->GetResourceCount(ConsumableId) < 1 || !WouldHaveEffect(*Row))
	{
		return;
	}

	// 6) Consommation atomique d'1 unité, puis effets. Si l'inventaire refuse, rien n'est appliqué.
	TMap<FName, int32> Cost;
	Cost.Add(ConsumableId, 1);
	if (!Inventory->ConsumeResources(Cost))
	{
		return;
	}
	LastConsumeTime = Now;

	ApplyConsumable(*Row);
	OnCorruptionChanged(); // Effets (PV max, endurance), états répliqués et interface.
}

void UEMBSurvivalComponent::ApplyConsumable(const FEMBConsumableRow& Row)
{
	using namespace EMBSurvivalDefaults;

	// Les valeurs de la table sont de la donnée de conception éditable : neutralisées si non finies.
	Hunger = FMath::Clamp(Hunger + SurvivalFiniteOrZero(Row.HungerDelta), 0.f, MaxGauge);
	Thirst = FMath::Clamp(Thirst + SurvivalFiniteOrZero(Row.ThirstDelta), 0.f, MaxGauge);

	if (Row.HealthPercent > 0.f && Stats)
	{
		Stats->Heal(Stats->GetMaxHealth() * FMath::Clamp(SurvivalFiniteOrZero(Row.HealthPercent), 0.f, 1.f));
	}

	const float CorruptionDelta = SurvivalFiniteOrZero(Row.CorruptionDelta);
	if (CorruptionDelta > 0.f)
	{
		AddCorruption(CorruptionDelta);
	}
	else if (CorruptionDelta < 0.f)
	{
		ReduceCorruption(-CorruptionDelta);
	}

	if (Row.bCuresIntoxication)
	{
		IntoxicationRemaining = 0.f;
	}
	const float Chance = FMath::Clamp(SurvivalFiniteOrZero(Row.IntoxicationChance), 0.f, 1.f);
	if (Chance > 0.f && Row.IntoxicationDuration > 0.f && FMath::FRand() < Chance)
	{
		// Non cumulable : la durée la plus longue l'emporte, l'intensité est celle de la dernière source.
		IntoxicationRemaining = FMath::Max(IntoxicationRemaining, FMath::Min(SurvivalFiniteOrZero(Row.IntoxicationDuration), MaxIntoxicationSeconds));
		IntoxicationDamagePerSecond = FMath::Clamp(SurvivalFiniteOrZero(Row.IntoxicationDamagePerSecond), 0.f, MaxIntoxicationDamagePerSecond);
	}

	if (Row.FireResistanceBonus > 0.f && Row.FireResistanceDuration > 0.f)
	{
		// Non cumulable : un nouvel usage remplace le bonus et rafraîchit la durée (jamais de somme).
		FireOilBonus = FMath::Clamp(SurvivalFiniteOrZero(Row.FireResistanceBonus), 0.f, MaxFireResistanceBonus);
		FireOilRemaining = FMath::Min(SurvivalFiniteOrZero(Row.FireResistanceDuration), MaxFireOilSeconds);
	}
}
