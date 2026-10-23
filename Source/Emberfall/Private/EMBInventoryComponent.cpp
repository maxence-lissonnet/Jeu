// Emberfall - inventaire de ressources minimal (Dev B, contrat 3.5).

#include "EMBInventoryComponent.h"

#include "Emberfall.h"
#include "GameFramework/Actor.h"
#include "Net/UnrealNetwork.h"

UEMBInventoryComponent::UEMBInventoryComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UEMBInventoryComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Inventaire privé : seul le joueur propriétaire reçoit le contenu (économie de bande passante et anti-triche).
	DOREPLIFETIME_CONDITION(UEMBInventoryComponent, Resources, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBInventoryComponent, CarryWeightBonus, COND_OwnerOnly);
}

void UEMBInventoryComponent::SetCarryWeightBonus(float NewBonus)
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	CarryWeightBonus = FMath::Max(0.f, NewBonus);
	// Rafraîchit l'encombrement côté serveur / hôte ; les clients passent par OnRep_CarryWeightBonus.
	OnInventoryChanged.Broadcast(NAME_None, 0);
}

void UEMBInventoryComponent::OnRep_CarryWeightBonus()
{
	OnInventoryChanged.Broadcast(NAME_None, 0);
}

void UEMBInventoryComponent::ExportResources(TArray<FEMBResourceStack>& OutResources) const
{
	OutResources = Resources;
}

void UEMBInventoryComponent::ImportResources(const TArray<FEMBResourceStack>& InResources)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	// Bornes : le nombre de ressources distinctes ne peut pas dépasser celui du référentiel (12), avec une marge.
	constexpr int32 MaxDistinctResources = 32;

	Resources.Reset();
	for (const FEMBResourceStack& In : InResources)
	{
		// Une sauvegarde peut être corrompue ou éditée à la main : on assainit tout. Seuls les identifiants connus
		// (poids unitaire > 0, table du référentiel) sont acceptés.
		if (In.ResourceId.IsNone() || In.Count <= 0 || GetUnitWeight(In.ResourceId) <= 0.f)
		{
			continue;
		}
		if (Resources.Num() >= MaxDistinctResources && FindEntryIndex(In.ResourceId) == INDEX_NONE)
		{
			break;
		}
		const int32 Index = FindEntryIndex(In.ResourceId);
		if (Index == INDEX_NONE)
		{
			FEMBResourceEntry& NewEntry = Resources.AddDefaulted_GetRef();
			NewEntry.ResourceId = In.ResourceId;
			NewEntry.Count = FMath::Min(In.Count, MaxStackPerResource);
		}
		else
		{
			const int64 Merged = static_cast<int64>(Resources[Index].Count) + static_cast<int64>(In.Count);
			Resources[Index].Count = static_cast<int32>(FMath::Min<int64>(Merged, MaxStackPerResource));
		}
	}

	// Rafraîchissement global (HUD) côté serveur ; les clients le reçoivent par OnRep_Resources.
	OnInventoryChanged.Broadcast(NAME_None, 0);
}

bool UEMBInventoryComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

int32 UEMBInventoryComponent::FindEntryIndex(FName ResourceId) const
{
	return Resources.IndexOfByPredicate([ResourceId](const FEMBResourceEntry& Entry)
	{
		return Entry.ResourceId == ResourceId;
	});
}

float UEMBInventoryComponent::GetUnitWeight(FName ResourceId)
{
	// Poids unitaires du référentiel (section 4.3). Table construite une seule fois.
	static const TMap<FName, float> UnitWeights = {
		{ TEXT("Wood"), 0.5f },
		{ TEXT("Stone"), 1.f },
		{ TEXT("Fiber"), 0.1f },
		{ TEXT("Flint"), 0.5f },
		{ TEXT("IronOre"), 2.f },
		{ TEXT("IronIngot"), 1.f },
		{ TEXT("BlackSteelIngot"), 1.f },
		{ TEXT("Obsidian"), 1.5f },
		{ TEXT("MoonSilver"), 1.f },
		{ TEXT("DragonScale"), 2.f },
		{ TEXT("DragonBone"), 3.f },
		{ TEXT("EmberHeart"), 5.f }
	};
	const float* Weight = UnitWeights.Find(ResourceId);
	return Weight ? *Weight : 0.f;
}

float UEMBInventoryComponent::GetCurrentWeight() const
{
	float Total = 0.f;
	for (const FEMBResourceEntry& Entry : Resources)
	{
		Total += GetUnitWeight(Entry.ResourceId) * static_cast<float>(Entry.Count);
	}
	return Total;
}

int32 UEMBInventoryComponent::GetResourceCount(FName ResourceId) const
{
	const int32 Index = FindEntryIndex(ResourceId);
	return Index != INDEX_NONE ? Resources[Index].Count : 0;
}

bool UEMBInventoryComponent::HasResources(const TMap<FName, int32>& Cost) const
{
	for (const TPair<FName, int32>& Pair : Cost)
	{
		if (Pair.Value < 0)
		{
			return false; // un coût négatif serait un gain déguisé : refusé
		}
		if (Pair.Value > 0 && GetResourceCount(Pair.Key) < Pair.Value)
		{
			return false;
		}
	}
	return true;
}

bool UEMBInventoryComponent::ConsumeResources(const TMap<FName, int32>& Cost)
{
	if (!HasAuthorityOwner())
	{
		return false;
	}

	// Passe 1 : vérification complète, aucune écriture. C'est ce qui rend l'opération atomique.
	if (!HasResources(Cost))
	{
		return false;
	}

	// Passe 2 : retrait. Ne peut plus échouer (le serveur est mono-thread pour la logique de jeu).
	TArray<FName, TInlineAllocator<8>> Changed;
	for (const TPair<FName, int32>& Pair : Cost)
	{
		if (Pair.Value <= 0)
		{
			continue;
		}
		const int32 Index = FindEntryIndex(Pair.Key);
		if (Index == INDEX_NONE)
		{
			continue;
		}
		Resources[Index].Count -= Pair.Value;
		Changed.Add(Pair.Key);
	}

	// Les entrées vides sont retirées pour garder le tableau répliqué minimal.
	Resources.RemoveAll([](const FEMBResourceEntry& Entry)
	{
		return Entry.Count <= 0;
	});

	for (const FName ResourceId : Changed)
	{
		OnInventoryChanged.Broadcast(ResourceId, GetResourceCount(ResourceId));
	}
	return true;
}

void UEMBInventoryComponent::AddResource(FName ResourceId, int32 Amount)
{
	if (!HasAuthorityOwner() || ResourceId.IsNone() || Amount <= 0)
	{
		return;
	}

	int32 Index = FindEntryIndex(ResourceId);
	if (Index == INDEX_NONE)
	{
		FEMBResourceEntry NewEntry;
		NewEntry.ResourceId = ResourceId;
		Index = Resources.Add(NewEntry);
	}

	// Calcul en 64 bits : pas de dépassement d'entier si un appelant passe une quantité énorme.
	const int64 NewCount = static_cast<int64>(Resources[Index].Count) + static_cast<int64>(Amount);
	Resources[Index].Count = static_cast<int32>(FMath::Min<int64>(NewCount, MaxStackPerResource));

	OnInventoryChanged.Broadcast(ResourceId, Resources[Index].Count);
}

void UEMBInventoryComponent::OnRep_Resources()
{
	// Rafraîchissement global côté client propriétaire (HUD, aperçu de construction).
	OnInventoryChanged.Broadcast(NAME_None, 0);
}
