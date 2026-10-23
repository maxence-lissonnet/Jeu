// Emberfall - inventaire de ressources minimal (Dev B, contrat 3.5).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMBInventoryComponent.generated.h"

/** Une ligne d'inventaire : identifiant de ressource (section 3.5 : Wood, Stone, IronIngot...) et quantité. */
USTRUCT(BlueprintType)
struct FEMBResourceEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory", SaveGame)
	FName ResourceId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory", SaveGame)
	int32 Count = 0;
};

/** Nom du type de paire dans le référentiel (section 7.4) : identique à FEMBResourceEntry. */
using FEMBResourceStack = FEMBResourceEntry;

/** Diffusé quand une ressource change (ResourceId) ou, après réplication, quand tout l'inventaire est rafraîchi (NAME_None). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnInventoryChanged, FName, ResourceId, int32, NewCount);

/**
 * Inventaire de ressources du joueur.
 *
 * Réseau : le contenu est stocké dans un TArray de paires (FEMBResourceEntry) et NON dans un TMap, car le moteur
 * ne sait pas répliquer les TMap (UPROPERTY(Replicated) sur un TMap est refusé). Les ressources étant peu nombreuses
 * (12 identifiants), une recherche linéaire dans le tableau est plus rapide qu'une table de hachage. Le tableau n'est
 * répliqué qu'à son propriétaire (COND_OwnerOnly) : l'inventaire est privé.
 *
 * Toute modification est autoritaire côté serveur ; les clients n'envoient jamais de quantités.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent))
class EMBERFALL_API UEMBInventoryComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBInventoryComponent();

	/** Quantité possédée (0 si absente). Utilisable côté client (valeur répliquée). */
	UFUNCTION(BlueprintPure, Category = "Inventory")
	int32 GetResourceCount(FName ResourceId) const;

	/** Vrai si toutes les quantités du coût sont disponibles. Un coût contenant une quantité négative est refusé. */
	bool HasResources(const TMap<FName, int32>& Cost) const;

	/** Serveur uniquement. Atomique (tout ou rien) : si une seule ressource manque, rien n'est retiré. */
	bool ConsumeResources(const TMap<FName, int32>& Cost);

	/** Serveur uniquement. Ignore les quantités <= 0. Plafonné à MaxStackPerResource. */
	void AddResource(FName ResourceId, int32 Amount);

	/** Poids total porté (ressources seules en v1), d'après les poids unitaires de la section 4.3 du référentiel. */
	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetCurrentWeight() const;

	/** Charge maximale portable (MaxCarryWeight). Le ratio poids/maximum pilote le montage de roulade (section 4.2). */
	UFUNCTION(BlueprintPure, Category = "Inventory")
	float GetMaxCarryWeight() const { return MaxCarryWeight + CarryWeightBonus; }

	/**
	 * Serveur : bonus de charge (passif « Bête de somme », +30 par rang). Répliqué au seul propriétaire pour que
	 * l'interface et le montage de roulade côté client voient le même maximum.
	 */
	UFUNCTION(BlueprintCallable, Category = "Inventory")
	void SetCarryWeightBonus(float NewBonus);

	/** Copie le contenu (sauvegarde). Utilisable côté serveur et côté client propriétaire. */
	void ExportResources(TArray<FEMBResourceStack>& OutResources) const;

	/**
	 * Serveur : remplace le contenu par celui d'une sauvegarde. Les entrées invalides (nom vide, quantité <= 0) sont
	 * ignorées, les doublons fusionnés, les quantités plafonnées à MaxStackPerResource.
	 */
	void ImportResources(const TArray<FEMBResourceStack>& InResources);

	/** Poids unitaire d'une ressource (0 si inconnue). Table statique de la section 4.3. */
	static float GetUnitWeight(FName ResourceId);

	UPROPERTY(BlueprintAssignable, Category = "Inventory")
	FEMBOnInventoryChanged OnInventoryChanged;

	/** Charge maximale (section 4.3 : 300). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Inventory", meta = (ClampMin = "1"))
	float MaxCarryWeight = 300.f;

	/** Plafond de quantité par ressource. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Inventory", meta = (ClampMin = "1"))
	int32 MaxStackPerResource = 9999;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Resources();

	/** Le maximum de charge change : l'interface rafraîchit l'encombrement. */
	UFUNCTION()
	void OnRep_CarryWeightBonus();

private:
	/** Index de l'entrée dans Resources, ou INDEX_NONE. */
	int32 FindEntryIndex(FName ResourceId) const;

	bool HasAuthorityOwner() const;

	UPROPERTY(ReplicatedUsing = OnRep_Resources)
	TArray<FEMBResourceEntry> Resources;

	UPROPERTY(ReplicatedUsing = OnRep_CarryWeightBonus)
	float CarryWeightBonus = 0.f;
};
