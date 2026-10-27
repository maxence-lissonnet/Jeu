// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBEquipmentComponent.h"
#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBStatsComponent.h"
#include "EMBSurvivalComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

namespace
{
	/** Remplace une valeur non finie (NaN, infini) par 0 : une ligne de table ne doit pas empoisonner les stats. */
	float EquipFiniteOrZero(float Value)
	{
		return FMath::IsFinite(Value) ? Value : 0.f;
	}

	/** Borne une valeur de table dans [Min, Max] en neutralisant les valeurs non finies. */
	float EquipSafeClamp(float Value, float Min, float Max)
	{
		return FMath::Clamp(EquipFiniteOrZero(Value), Min, Max);
	}

	bool EquipCombatStatsEqual(const FEMBEquipmentCombatStats& A, const FEMBEquipmentCombatStats& B)
	{
		return A.WeaponDamageType == B.WeaponDamageType
			&& FMath::IsNearlyEqual(A.WeaponBaseDamage, B.WeaponBaseDamage)
			&& FMath::IsNearlyEqual(A.AttackStaminaMultiplier, B.AttackStaminaMultiplier)
			&& FMath::IsNearlyEqual(A.PoiseDamageMultiplier, B.PoiseDamageMultiplier)
			&& FMath::IsNearlyEqual(A.GuardPhysical, B.GuardPhysical)
			&& FMath::IsNearlyEqual(A.GuardElemental, B.GuardElemental)
			&& FMath::IsNearlyEqual(A.GuardCorruption, B.GuardCorruption)
			&& FMath::IsNearlyEqual(A.ArmorPhysicalReduction, B.ArmorPhysicalReduction)
			&& FMath::IsNearlyEqual(A.ArmorFireReduction, B.ArmorFireReduction);
	}

	/** Indices des emplacements d'armure dans le tableau Equipped (Head, Chest, Hands, Legs). */
	constexpr int32 EquipArmorFirstIndex = 2;
	constexpr int32 EquipArmorLastIndex = 5;
}

UEMBEquipmentComponent::UEMBEquipmentComponent()
{
	// Pas de Tick : tout est piloté par des événements (RPC, coups, récolte).
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);

	ItemTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_Items.DT_Items")));
	Equipped.SetNum(EMBItemDefaults::NumEquipSlots);
}

// ---------------------------------------------------------------- Cycle de vie

void UEMBEquipmentComponent::BeginPlay()
{
	Super::BeginPlay();

	EnsureItemTable();
	if (Equipped.Num() != EMBItemDefaults::NumEquipSlots)
	{
		Equipped.SetNum(EMBItemDefaults::NumEquipSlots);
	}

	if (!HasAuthorityOwner())
	{
		return; // Les clients reçoivent l'état répliqué ; seul le serveur calcule et pousse les stats.
	}

	ResolveDependencies();
	RefreshDerivedStats(true);
	UpdateVisibleIds();

	// L'ordre de BeginPlay entre composants du même acteur n'est pas garanti : second envoi au tick suivant.
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(this, &UEMBEquipmentComponent::HandleDeferredPush);
	}
}

void UEMBEquipmentComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsValid(Combat) && bEventsBound)
	{
		Combat->OnHitLandedServer.RemoveDynamic(this, &UEMBEquipmentComponent::HandleHitLanded);
		Combat->OnHitReceived.RemoveDynamic(this, &UEMBEquipmentComponent::HandleHitReceived);
	}
	bEventsBound = false;

	Super::EndPlay(EndPlayReason);
}

void UEMBEquipmentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Inventaire privé : propriétaire seul.
	DOREPLIFETIME_CONDITION(UEMBEquipmentComponent, Bag, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBEquipmentComponent, Equipped, COND_OwnerOnly);

	// Apparence : visible de tous (deux identifiants).
	DOREPLIFETIME(UEMBEquipmentComponent, VisibleMainHandId);
	DOREPLIFETIME(UEMBEquipmentComponent, VisibleOffHandId);
}

bool UEMBEquipmentComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

void UEMBEquipmentComponent::EnsureItemTable() const
{
	if (LoadedItems || ItemTable.IsNull())
	{
		return;
	}
	UEMBEquipmentComponent* MutableThis = const_cast<UEMBEquipmentComponent*>(this); // Chargement paresseux, sans changement d'état observable.
	MutableThis->LoadedItems = ItemTable.LoadSynchronous();
	if (!LoadedItems)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBEquipmentComponent: DT_Items introuvable (%s) : aucun objet utilisable."), *ItemTable.ToString());
	}
}

void UEMBEquipmentComponent::ResolveDependencies()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	if (!Combat)
	{
		Combat = Owner->FindComponentByClass<UEMBCombatComponent>();
	}
	if (!Stats)
	{
		Stats = Owner->FindComponentByClass<UEMBStatsComponent>();
	}
	if (!Survival)
	{
		Survival = Owner->FindComponentByClass<UEMBSurvivalComponent>();
	}

	if (Combat && !bEventsBound && HasAuthorityOwner())
	{
		Combat->OnHitLandedServer.AddUniqueDynamic(this, &UEMBEquipmentComponent::HandleHitLanded);
		Combat->OnHitReceived.AddUniqueDynamic(this, &UEMBEquipmentComponent::HandleHitReceived);
		bEventsBound = true;
	}
}

void UEMBEquipmentComponent::HandleDeferredPush()
{
	if (HasAuthorityOwner())
	{
		RefreshDerivedStats(true);
	}
}

// ---------------------------------------------------------------- Données

const FEMBItemRow* UEMBEquipmentComponent::FindItemRow(FName ItemId) const
{
	EnsureItemTable(); // Chargement à la demande : valable avant le BeginPlay (import de sauvegarde) et sur tout appelant.
	if (!LoadedItems || ItemId.IsNone())
	{
		return nullptr;
	}
	return LoadedItems->FindRow<FEMBItemRow>(ItemId, TEXT("UEMBEquipmentComponent::FindItemRow"), /*bWarnIfRowMissing*/ false);
}

EEMBEquipSlot UEMBEquipmentComponent::ResolveSlot(const FEMBItemRow& Row)
{
	switch (Row.ItemType)
	{
	case EEMBItemType::Tool:
	case EEMBItemType::Weapon:
		return EEMBEquipSlot::MainHand;
	case EEMBItemType::Shield:
		return EEMBEquipSlot::OffHand;
	case EEMBItemType::Armor:
		switch (Row.Slot)
		{
		case EEMBEquipSlot::Head:
		case EEMBEquipSlot::Chest:
		case EEMBEquipSlot::Hands:
		case EEMBEquipSlot::Legs:
			return Row.Slot;
		default:
			return EEMBEquipSlot::None;
		}
	default:
		return EEMBEquipSlot::None;
	}
}

int32 UEMBEquipmentComponent::SlotToIndex(EEMBEquipSlot Slot)
{
	const int32 Raw = static_cast<int32>(Slot);
	const int32 Index = Raw - 1;
	return (Index >= 0 && Index < EMBItemDefaults::NumEquipSlots) ? Index : INDEX_NONE;
}

bool UEMBEquipmentComponent::IsInstanceBroken(const FEMBItemInstance& Instance) const
{
	const FEMBItemRow* Row = FindItemRow(Instance.ItemId);
	return Row && Row->MaxDurability > 0 && Instance.Durability <= 0.f;
}

const FEMBItemRow* UEMBEquipmentComponent::GetActiveRow(EEMBEquipSlot Slot) const
{
	const int32 Index = SlotToIndex(Slot);
	if (!Equipped.IsValidIndex(Index))
	{
		return nullptr;
	}
	const FEMBItemInstance& Instance = Equipped[Index];
	const FEMBItemRow* Row = FindItemRow(Instance.ItemId);
	if (!Row || ResolveSlot(*Row) == EEMBEquipSlot::None)
	{
		return nullptr;
	}
	if (Row->MaxDurability > 0 && Instance.Durability <= 0.f)
	{
		return nullptr; // Cassé : reste équipé, sans effet.
	}
	return Row;
}

FEMBItemInstance UEMBEquipmentComponent::GetEquippedItem(EEMBEquipSlot Slot) const
{
	const int32 Index = SlotToIndex(Slot);
	return Equipped.IsValidIndex(Index) ? Equipped[Index] : FEMBItemInstance();
}

int32 UEMBEquipmentComponent::GetBagFreeSlots() const
{
	return FMath::Max(0, EMBItemDefaults::MaxItemInstances - Bag.Num());
}

int32 UEMBEquipmentComponent::CountDamagedInstances(FName ItemId) const
{
	const FEMBItemRow* Row = FindItemRow(ItemId);
	if (!Row || Row->MaxDurability <= 0)
	{
		return 0;
	}
	const float Max = static_cast<float>(Row->MaxDurability);
	int32 Count = 0;
	for (const FEMBItemInstance& Instance : Bag)
	{
		Count += (Instance.ItemId == ItemId && Instance.Durability < Max) ? 1 : 0;
	}
	for (const FEMBItemInstance& Instance : Equipped)
	{
		Count += (Instance.ItemId == ItemId && Instance.Durability < Max) ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------- Contrat : outil, poids, ajout

FEMBEquippedToolInfo UEMBEquipmentComponent::GetEquippedTool() const
{
	FEMBEquippedToolInfo Info; // Mains nues : None, tier 0, 5 dégâts.
	Info.HarvestDamage = EMBItemDefaults::BareHandsHarvestDamage;

	const int32 Index = SlotToIndex(EEMBEquipSlot::MainHand);
	if (!Equipped.IsValidIndex(Index))
	{
		return Info;
	}
	const FEMBItemInstance& Main = Equipped[Index];
	const FEMBItemRow* Row = FindItemRow(Main.ItemId);
	if (!Row || Row->ItemType != EEMBItemType::Tool)
	{
		return Info; // Une arme ne récolte pas : elle compte comme les mains nues.
	}
	if (IsInstanceBroken(Main))
	{
		Info.bBroken = true; // Outil cassé : sans effet, donc mains nues avec l'indicateur de casse.
		return Info;
	}

	Info.ToolType = Row->ToolType;
	Info.Tier = FMath::Max(0, Row->Tier);
	Info.HarvestDamage = FMath::Max(0.f, EquipFiniteOrZero(Row->HarvestDamage));
	return Info;
}

void UEMBEquipmentComponent::NotifyToolUsed()
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	bool bChanged = false;
	const bool bBroke = WearSlot(SlotToIndex(EEMBEquipSlot::MainHand), 1.f, bChanged);
	if (bChanged)
	{
		NotifyChanged();
	}
	if (bBroke)
	{
		RefreshDerivedStats(false);
	}
}

float UEMBEquipmentComponent::GetTotalWeight() const
{
	float Total = 0.f;
	for (const FEMBItemInstance& Instance : Bag)
	{
		if (const FEMBItemRow* Row = FindItemRow(Instance.ItemId))
		{
			Total += FMath::Max(0.f, EquipFiniteOrZero(Row->Weight));
		}
	}
	for (const FEMBItemInstance& Instance : Equipped)
	{
		if (const FEMBItemRow* Row = FindItemRow(Instance.ItemId))
		{
			Total += FMath::Max(0.f, EquipFiniteOrZero(Row->Weight));
		}
	}
	return Total;
}

bool UEMBEquipmentComponent::AddItemInstance(FName ItemId, float Durability)
{
	if (!HasAuthorityOwner())
	{
		return false;
	}
	EnsureItemTable();

	const FEMBItemRow* Row = FindItemRow(ItemId);
	if (!Row || Bag.Num() >= EMBItemDefaults::MaxItemInstances)
	{
		return false;
	}

	const float Max = static_cast<float>(FMath::Max(0, Row->MaxDurability));
	FEMBItemInstance NewItem;
	NewItem.ItemId = ItemId;
	NewItem.Durability = (!FMath::IsFinite(Durability) || Durability < 0.f) ? Max : FMath::Min(Durability, Max);
	Bag.Add(NewItem);

	NotifyChanged();
	return true;
}

bool UEMBEquipmentComponent::RepairFirstInstance(FName ItemId)
{
	if (!HasAuthorityOwner())
	{
		return false;
	}
	const FEMBItemRow* Row = FindItemRow(ItemId);
	if (!Row || Row->MaxDurability <= 0)
	{
		return false;
	}
	const float Max = static_cast<float>(Row->MaxDurability);

	// Deux passes : d'abord la première instance cassée (sac puis équipement), sinon la première abîmée.
	FEMBItemInstance* Target = nullptr;
	for (int32 Pass = 0; Pass < 2 && !Target; ++Pass)
	{
		auto Matches = [&](const FEMBItemInstance& Instance)
		{
			return Instance.ItemId == ItemId && (Pass == 0 ? Instance.Durability <= 0.f : Instance.Durability < Max);
		};
		for (FEMBItemInstance& Instance : Bag)
		{
			if (Matches(Instance))
			{
				Target = &Instance;
				break;
			}
		}
		if (!Target)
		{
			for (FEMBItemInstance& Instance : Equipped)
			{
				if (Matches(Instance))
				{
					Target = &Instance;
					break;
				}
			}
		}
	}
	if (!Target)
	{
		return false;
	}

	Target->Durability = Max;
	NotifyChanged();
	RefreshDerivedStats(false); // Un objet réparé retrouve ses effets.
	return true;
}

// ---------------------------------------------------------------- Équiper / déséquiper

bool UEMBEquipmentComponent::ConsumeRpcBudget()
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (Now - RpcWindowStart >= 1.0)
	{
		RpcWindowStart = Now;
		RpcWindowCount = 0;
	}
	return ++RpcWindowCount <= 10; // ~10 demandes par seconde : l'excédent est ignoré.
}

bool UEMBEquipmentComponent::IsCombatIdle() const
{
	return !Combat || Combat->GetCurrentState() == EEMBCombatState::Idle;
}

bool UEMBEquipmentComponent::RequestEquip_Validate(int32 BagIndex, FName ItemId)
{
	return BagIndex >= 0 && BagIndex < EMBItemDefaults::MaxItemInstances;
}

void UEMBEquipmentComponent::RequestEquip_Implementation(int32 BagIndex, FName ItemId)
{
	if (!HasAuthorityOwner() || !ConsumeRpcBudget())
	{
		return;
	}
	EnsureItemTable();
	ResolveDependencies();

	if ((Stats && Stats->IsDead()) || !IsCombatIdle())
	{
		return;
	}
	if (!Bag.IsValidIndex(BagIndex) || Bag[BagIndex].ItemId != ItemId)
	{
		return; // Index périmé ou objet différent (le sac a changé entre-temps) : ignoré sans sanction.
	}

	const FEMBItemRow* Row = FindItemRow(Bag[BagIndex].ItemId);
	if (!Row)
	{
		return;
	}
	const int32 SlotIndex = SlotToIndex(ResolveSlot(*Row));
	if (!Equipped.IsValidIndex(SlotIndex))
	{
		return;
	}

	const FEMBItemInstance Incoming = Bag[BagIndex];
	const FEMBItemInstance Previous = Equipped[SlotIndex];
	Equipped[SlotIndex] = Incoming;
	if (Previous.ItemId.IsNone())
	{
		Bag.RemoveAt(BagIndex);
	}
	else
	{
		Bag[BagIndex] = Previous; // Échange : le sac ne grossit pas.
	}

	NotifyChanged();
	RefreshDerivedStats(false);
}

bool UEMBEquipmentComponent::RequestUnequip_Validate(EEMBEquipSlot Slot)
{
	return static_cast<uint8>(Slot) <= static_cast<uint8>(EEMBEquipSlot::Legs);
}

void UEMBEquipmentComponent::RequestUnequip_Implementation(EEMBEquipSlot Slot)
{
	if (!HasAuthorityOwner() || !ConsumeRpcBudget())
	{
		return;
	}
	ResolveDependencies();
	if (!IsCombatIdle())
	{
		return;
	}
	const int32 SlotIndex = SlotToIndex(Slot);
	if (!Equipped.IsValidIndex(SlotIndex) || Equipped[SlotIndex].ItemId.IsNone())
	{
		return;
	}
	if (Bag.Num() >= EMBItemDefaults::MaxItemInstances)
	{
		return; // Sac plein : l'objet reste équipé.
	}

	Bag.Add(Equipped[SlotIndex]);
	Equipped[SlotIndex] = FEMBItemInstance();

	NotifyChanged();
	RefreshDerivedStats(false);
}

// ---------------------------------------------------------------- Durabilité

bool UEMBEquipmentComponent::WearSlot(int32 SlotIndex, float Amount, bool& bOutChanged)
{
	bOutChanged = false;
	if (!Equipped.IsValidIndex(SlotIndex))
	{
		return false;
	}
	FEMBItemInstance& Instance = Equipped[SlotIndex];
	const FEMBItemRow* Row = FindItemRow(Instance.ItemId);
	if (!Row || Row->MaxDurability <= 0 || Instance.Durability <= 0.f)
	{
		return false; // Rien d'équipé, inusable ou déjà cassé.
	}

	Instance.Durability = FMath::Max(0.f, Instance.Durability - Amount);
	bOutChanged = true;
	return Instance.Durability <= 0.f;
}

void UEMBEquipmentComponent::HandleHitLanded(AActor* Target, EEMBHitResult Result)
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	switch (Result)
	{
	case EEMBHitResult::Hit:
	case EEMBHitResult::Killed:
	case EEMBHitResult::Blocked:
	case EEMBHitResult::GuardBroken:
		break; // Contact réel avec la cible : l'arme s'use.
	default:
		return; // Esquivé, paré, ignoré (nœud de récolte compris) : pas d'usure ici.
	}

	bool bChanged = false;
	const bool bBroke = WearSlot(SlotToIndex(EEMBEquipSlot::MainHand), 1.f, bChanged);
	if (bChanged)
	{
		NotifyChanged();
	}
	if (bBroke)
	{
		RefreshDerivedStats(false);
	}
}

void UEMBEquipmentComponent::HandleHitReceived(const FEMBHitData& Hit, EEMBHitResult Result)
{
	if (!HasAuthorityOwner() || Hit.bDamageOverTime)
	{
		return; // Les dégâts continus (lave, brûlure) n'usent rien.
	}

	bool bAnyChanged = false;
	bool bAnyBroke = false;

	switch (Result)
	{
	case EEMBHitResult::Blocked:
	case EEMBHitResult::GuardBroken:
	{
		bool bChanged = false;
		bAnyBroke = WearSlot(SlotToIndex(EEMBEquipSlot::OffHand), 1.f, bChanged);
		bAnyChanged = bChanged;
		break;
	}
	case EEMBHitResult::Hit:
	case EEMBHitResult::Killed:
		for (int32 Index = EquipArmorFirstIndex; Index <= EquipArmorLastIndex; ++Index)
		{
			bool bChanged = false;
			bAnyBroke |= WearSlot(Index, 1.f, bChanged);
			bAnyChanged |= bChanged;
		}
		break;
	default:
		return; // Esquive, parade, coup ignoré : aucune usure.
	}

	if (bAnyChanged)
	{
		NotifyChanged();
	}
	if (bAnyBroke)
	{
		RefreshDerivedStats(false);
	}
}

// ---------------------------------------------------------------- Stats dérivées

FEMBEquipmentCombatStats UEMBEquipmentComponent::ComputeDerivedStats(float& OutPoiseBonus, float& OutInsulation) const
{
	// Départ : mains nues, sans bouclier (le défaut de la structure est l'épée de référence avec bouclier).
	FEMBEquipmentCombatStats Out;
	Out.WeaponBaseDamage = EMBItemDefaults::BareHandsDamage;
	Out.WeaponDamageType = EEMBDamageType::Physical;
	Out.AttackStaminaMultiplier = 1.f;
	Out.PoiseDamageMultiplier = 1.f;
	Out.GuardPhysical = EMBItemDefaults::BareGuardPhysical;
	Out.GuardElemental = EMBItemDefaults::BareGuardElemental;
	Out.GuardCorruption = EMBItemDefaults::BareGuardCorruption;
	Out.ArmorPhysicalReduction = 0.f;
	Out.ArmorFireReduction = 0.f;
	OutPoiseBonus = 0.f;
	OutInsulation = 0.f;

	// Main droite : arme, ou outil tenu comme arme modeste.
	if (const FEMBItemRow* Main = GetActiveRow(EEMBEquipSlot::MainHand))
	{
		if (Main->ItemType == EEMBItemType::Weapon || Main->ItemType == EEMBItemType::Tool)
		{
			const float Damage = EquipFiniteOrZero(Main->WeaponBaseDamage);
			Out.WeaponBaseDamage = Damage > 0.f ? Damage : EMBItemDefaults::BareHandsDamage;
			Out.WeaponDamageType = Main->WeaponDamageType;
			Out.AttackStaminaMultiplier = EquipSafeClamp(Main->AttackStaminaMultiplier, 0.1f, 5.f);
			Out.PoiseDamageMultiplier = EquipSafeClamp(Main->PoiseDamageMultiplier, 0.f, 5.f);
		}
	}

	// Main gauche : un bouclier valide remplace les valeurs de garde.
	if (const FEMBItemRow* Off = GetActiveRow(EEMBEquipSlot::OffHand))
	{
		if (Off->ItemType == EEMBItemType::Shield)
		{
			Out.GuardPhysical = EquipSafeClamp(Off->GuardPhysical, 0.f, 1.f);
			Out.GuardElemental = EquipSafeClamp(Off->GuardElemental, 0.f, 1.f);
			Out.GuardCorruption = EquipSafeClamp(Off->GuardCorruption, 0.f, 1.f);
		}
	}

	// Armures : somme des pièces non cassées, réductions plafonnées.
	float Physical = 0.f;
	float Fire = 0.f;
	for (int32 Index = EquipArmorFirstIndex; Index <= EquipArmorLastIndex; ++Index)
	{
		const EEMBEquipSlot Slot = static_cast<EEMBEquipSlot>(Index + 1);
		const FEMBItemRow* Row = GetActiveRow(Slot);
		if (!Row || Row->ItemType != EEMBItemType::Armor)
		{
			continue;
		}
		Physical += EquipSafeClamp(Row->ArmorPhysical, 0.f, 1.f);
		Fire += EquipSafeClamp(Row->ArmorFire, 0.f, 1.f);
		OutPoiseBonus += FMath::Max(0.f, EquipFiniteOrZero(Row->ArmorPoise));
		OutInsulation += EquipFiniteOrZero(Row->Insulation);
	}
	Out.ArmorPhysicalReduction = FMath::Min(Physical, EMBItemDefaults::ArmorReductionCap);
	Out.ArmorFireReduction = FMath::Min(Fire, EMBItemDefaults::ArmorReductionCap);
	return Out;
}

void UEMBEquipmentComponent::RefreshDerivedStats(bool bForce)
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	EnsureItemTable();
	ResolveDependencies();

	float Poise = 0.f;
	float Insulation = 0.f;
	const FEMBEquipmentCombatStats NewStats = ComputeDerivedStats(Poise, Insulation);

	if (Combat && (bForce || !bCombatPushed || !EquipCombatStatsEqual(NewStats, PushedCombat)))
	{
		Combat->SetEquipmentCombatStats(NewStats);
		PushedCombat = NewStats;
		bCombatPushed = true;
	}
	if (Stats && (bForce || !bPoisePushed || !FMath::IsNearlyEqual(Poise, PushedPoise)))
	{
		Stats->SetEquipmentPoiseBonus(Poise);
		PushedPoise = Poise;
		bPoisePushed = true;
	}
	if (Survival && (bForce || !bInsulationPushed || !FMath::IsNearlyEqual(Insulation, PushedInsulation)))
	{
		Survival->SetInsulation(Insulation);
		PushedInsulation = Insulation;
		bInsulationPushed = true;
	}
}

// ---------------------------------------------------------------- Notification et réplication

void UEMBEquipmentComponent::UpdateVisibleIds()
{
	const FName Main = Equipped.IsValidIndex(0) ? Equipped[0].ItemId : NAME_None;
	const FName Off = Equipped.IsValidIndex(1) ? Equipped[1].ItemId : NAME_None;
	// Affectation conditionnelle : ne marque la propriété « sale » que si elle change vraiment.
	bool bVisibleChanged = false;
	if (VisibleMainHandId != Main)
	{
		VisibleMainHandId = Main;
		bVisibleChanged = true;
	}
	if (VisibleOffHandId != Off)
	{
		VisibleOffHandId = Off;
		bVisibleChanged = true;
	}
	// Sur l'autorité (hôte d'une partie en écoute) OnRep n'est pas appelé : on diffuse ici.
	if (bVisibleChanged)
	{
		OnVisibleEquipmentChanged.Broadcast(VisibleMainHandId, VisibleOffHandId);
	}
}

void UEMBEquipmentComponent::NotifyChanged()
{
	if (HasAuthorityOwner())
	{
		UpdateVisibleIds();
	}
	OnEquipmentChanged.Broadcast();
}

void UEMBEquipmentComponent::OnRep_Bag()
{
	OnEquipmentChanged.Broadcast();
}

void UEMBEquipmentComponent::OnRep_Equipped()
{
	OnEquipmentChanged.Broadcast();
}

void UEMBEquipmentComponent::OnRep_Visible()
{
	OnVisibleEquipmentChanged.Broadcast(VisibleMainHandId, VisibleOffHandId);
}

// ---------------------------------------------------------------- Sauvegarde

void UEMBEquipmentComponent::ExportSaveData(FEMBEquipmentSaveData& Out) const
{
	Out.BagItems = Bag;
	Out.EquippedItems.Reset();
	for (const FEMBItemInstance& Instance : Equipped)
	{
		if (!Instance.ItemId.IsNone())
		{
			Out.EquippedItems.Add(Instance);
		}
	}
}

bool UEMBEquipmentComponent::SanitizeInstance(const FEMBItemInstance& In, FEMBItemInstance& Out) const
{
	const FEMBItemRow* Row = FindItemRow(In.ItemId);
	if (!Row)
	{
		return false; // Identifiant inconnu (table modifiée, fichier altéré) : ignoré.
	}
	const float Max = static_cast<float>(FMath::Max(0, Row->MaxDurability));
	Out.ItemId = In.ItemId;
	Out.Durability = FMath::IsFinite(In.Durability) ? FMath::Clamp(In.Durability, 0.f, Max) : Max;
	return true;
}

void UEMBEquipmentComponent::ImportSaveData(const FEMBEquipmentSaveData& In)
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	EnsureItemTable();
	if (!LoadedItems)
	{
		// Sans table, tout serait jugé « inconnu » : on garde l'état courant plutôt que de vider le sac du joueur.
		UE_LOG(LogEmberfall, Error, TEXT("UEMBEquipmentComponent::ImportSaveData: DT_Items indisponible, import annule."));
		return;
	}

	TArray<FEMBItemInstance> NewBag;
	TArray<FEMBItemInstance> NewEquipped;
	NewEquipped.SetNum(EMBItemDefaults::NumEquipSlots);

	// Sac d'abord (l'ordre est conservé), borné à 40.
	for (const FEMBItemInstance& Saved : In.BagItems)
	{
		FEMBItemInstance Clean;
		if (NewBag.Num() < EMBItemDefaults::MaxItemInstances && SanitizeInstance(Saved, Clean))
		{
			NewBag.Add(Clean);
		}
	}

	// Équipement : un objet par emplacement déduit de la ligne ; surplus ou doublon renvoyé au sac s'il reste de la place.
	for (const FEMBItemInstance& Saved : In.EquippedItems)
	{
		FEMBItemInstance Clean;
		if (!SanitizeInstance(Saved, Clean))
		{
			continue;
		}
		const FEMBItemRow* Row = FindItemRow(Clean.ItemId);
		const int32 SlotIndex = Row ? SlotToIndex(ResolveSlot(*Row)) : INDEX_NONE;
		if (NewEquipped.IsValidIndex(SlotIndex) && NewEquipped[SlotIndex].ItemId.IsNone())
		{
			NewEquipped[SlotIndex] = Clean;
		}
		else if (NewBag.Num() < EMBItemDefaults::MaxItemInstances)
		{
			NewBag.Add(Clean);
		}
	}

	Bag = MoveTemp(NewBag);
	Equipped = MoveTemp(NewEquipped);

	NotifyChanged();
	RefreshDerivedStats(true);
}
