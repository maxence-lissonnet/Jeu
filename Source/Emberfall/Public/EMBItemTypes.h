// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EMBCombatTypes.h"
#include "EMBItemTypes.generated.h"

// ------------------------------------------------------------------------------------------------
// Constantes (référentiel 00_Constantes_et_Contrats.md, section 14)
// ------------------------------------------------------------------------------------------------

/**
 * Valeurs de gameplay des objets, de l'équipement et de l'artisanat. Pour modifier une valeur : changer le
 * référentiel d'abord, puis ce fichier. Les valeurs marquées (*) ne sont pas fixées par le référentiel.
 */
namespace EMBItemDefaults
{
	/** Taille du sac d'objets durables (section 14). */
	inline constexpr int32 MaxItemInstances = 40;
	/** MainHand, OffHand, Head, Chest, Hands, Legs. */
	inline constexpr int32 NumEquipSlots = 6;

	// Mains nues (aucune arme valide en main droite, aucun bouclier valide)
	inline constexpr float BareHandsDamage = 20.f;
	inline constexpr float BareGuardPhysical = 0.50f;
	inline constexpr float BareGuardElemental = 0.25f;
	inline constexpr float BareGuardCorruption = 0.10f;
	/** Dégâts de récolte à mains nues (section 13). */
	inline constexpr float BareHandsHarvestDamage = 5.f;

	/** Plafond de la réduction physique et feu cumulée des armures. */
	inline constexpr float ArmorReductionCap = 0.60f;

	/** Entrées maximum de la file de fabrication. */
	inline constexpr int32 MaxQueueEntries = 10;
	/** Distance maximale (cm) entre le joueur et l'atelier requis. */
	inline constexpr float StationRadius = 600.f;
	/** Part du coût de fabrication demandée pour une réparation (arrondi supérieur, appliqué dans DT_Recipes). */
	inline constexpr float RepairCostFraction = 0.25f;

	// (*) Valeurs non fixées par le référentiel
	/** Quantité maximale par demande de fabrication (borne anti-abus du RPC). */
	inline constexpr int32 MaxCraftCountPerRequest = 99;
	/** Quantité maximale d'un ingrédient dans une ligne de recette (borne de validité de la table). */
	inline constexpr int32 MaxIngredientAmount = 9999;
	/** Quantité totale maximale d'un ingrédient pour une demande (ligne × nombre d'exemplaires). */
	inline constexpr int32 MaxTotalIngredientAmount = 100000;
	/** Période du timer serveur de la file de fabrication (le composant n'utilise pas de Tick). */
	inline constexpr float CraftTickInterval = 0.25f;
	/** Unités de fabrication livrables au plus par passage du timer (évite une boucle sans fin sur une table erronée). */
	inline constexpr int32 MaxCraftStepsPerTick = 16;
}

// ------------------------------------------------------------------------------------------------
// Énumérations
// ------------------------------------------------------------------------------------------------

UENUM(BlueprintType)
enum class EEMBItemType : uint8
{
	Tool,
	Weapon,
	Shield,
	Armor
};

UENUM(BlueprintType)
enum class EEMBEquipSlot : uint8
{
	None,
	MainHand,
	OffHand,
	Head,
	Chest,
	Hands,
	Legs
};

UENUM(BlueprintType)
enum class EEMBToolType : uint8
{
	None,
	Axe,
	Pickaxe,
	Knife
};

/** Résultat d'une demande de fabrication (retour d'interface via UEMBCraftingComponent::OnCraftRejected). */
UENUM(BlueprintType)
enum class EEMBCraftResult : uint8
{
	Ok,
	UnknownRecipe,
	InvalidRequest,
	InvalidRecipe,
	MissingGlyph,
	MissingIngredients,
	QueueFull,
	BagFull,
	NothingToRepair
};

// ------------------------------------------------------------------------------------------------
// Objets
// ------------------------------------------------------------------------------------------------

/** Ligne de DT_Items : définition d'un objet durable (nom de ligne = identifiant d'objet). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBItemRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	EEMBItemType ItemType = EEMBItemType::Tool;

	/** Emplacement visé. Le composant le déduit du type pour les outils, armes et boucliers ; seul compte ici celui des armures. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	EEMBEquipSlot Slot = EEMBEquipSlot::None;

	/** T1 à T4. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item", meta = (ClampMin = "0"))
	int32 Tier = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item", meta = (ClampMin = "0"))
	float Weight = 1.f;

	/** Durabilité maximale ; 0 ou moins = objet inusable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item", meta = (ClampMin = "0"))
	int32 MaxDurability = 100;

	// ---- Outils ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool")
	EEMBToolType ToolType = EEMBToolType::None;

	/** Dégâts de récolte par coup (section 13 : 25 / 40 / 55 / 75 selon le tier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool", meta = (ClampMin = "0"))
	float HarvestDamage = 0.f;

	// ---- Armes (et outils tenus comme arme) ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon", meta = (ClampMin = "0"))
	float WeaponBaseDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon")
	EEMBDamageType WeaponDamageType = EEMBDamageType::Physical;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon", meta = (ClampMin = "0"))
	float AttackStaminaMultiplier = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weapon", meta = (ClampMin = "0"))
	float PoiseDamageMultiplier = 1.f;

	// ---- Boucliers : remplacent les valeurs de garde des mains nues ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shield", meta = (ClampMin = "0", ClampMax = "1"))
	float GuardPhysical = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shield", meta = (ClampMin = "0", ClampMax = "1"))
	float GuardElemental = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shield", meta = (ClampMin = "0", ClampMax = "1"))
	float GuardCorruption = 0.f;

	// ---- Armures ----
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Armor", meta = (ClampMin = "0", ClampMax = "1"))
	float ArmorPhysical = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Armor", meta = (ClampMin = "0", ClampMax = "1"))
	float ArmorFire = 0.f;

	/** Bonus de poise maximale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Armor", meta = (ClampMin = "0"))
	float ArmorPoise = 0.f;

	/** Isolation thermique en °C, additionnée sur toutes les pièces portées. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Armor")
	float Insulation = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	FText Description;
};

/** Un exemplaire d'objet durable : identifiant de ligne DT_Items + durabilité courante. Défauts neutres (sauvegarde). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBItemInstance
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item", SaveGame)
	FName ItemId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item", SaveGame)
	float Durability = 0.f;
};

/** Outil tenu en main droite, tel que lu par la récolte (contrat section 12). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBEquippedToolInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool")
	EEMBToolType ToolType = EEMBToolType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool")
	int32 Tier = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool")
	float HarvestDamage = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tool")
	bool bBroken = false;
};

// ------------------------------------------------------------------------------------------------
// Artisanat
// ------------------------------------------------------------------------------------------------

/** Ligne de DT_Recipes (nom de ligne = identifiant de recette). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBRecipeRow : public FTableRowBase
{
	GENERATED_BODY()

	/** Ressource (bOutputIsItem = faux) ou objet de DT_Items (bOutputIsItem = vrai) produit. Ignoré pour une réparation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	FName OutputId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe", meta = (ClampMin = "1"))
	int32 OutputCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	bool bOutputIsItem = false;

	/** Ressources consommées par exemplaire fabriqué. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	TMap<FName, int32> Ingredients;

	/** Ligne de DT_BuildPieces (Workbench, Forge, Smelter, Campfire, DraconicForge) ; vide = à la main. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	FName RequiredStation = NAME_None;

	/** Glyphe requis (DT_Glyphs) ; vide = aucun. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	FName RequiredGlyph = NAME_None;

	/** Durée de fabrication d'un exemplaire, en secondes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe", meta = (ClampMin = "0"))
	float CraftTime = 5.f;

	/** XP accordée par exemplaire fabriqué. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe", meta = (ClampMin = "0"))
	int32 XP = 0;

	/** Recette de réparation : restaure l'objet RepairTargetItem au lieu de produire quoi que ce soit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	bool bIsRepair = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Recipe")
	FName RepairTargetItem = NAME_None;
};

/**
 * Une entrée de la file de fabrication : RecipeId fabriqué Remaining fois. Seule la première entrée progresse.
 * Réplication : AccumulatedSeconds est la progression de l'exemplaire en cours AU MOMENT RunningSinceServerTime ;
 * le client calcule la progression courante depuis l'heure serveur (ces champs ne changent qu'à un démarrage, une
 * pause ou une livraison, jamais à chaque image).
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBCraftQueueEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting", SaveGame)
	FName RecipeId = NAME_None;

	/** Exemplaires restant à fabriquer (l'exemplaire en cours compris). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting", SaveGame)
	int32 Remaining = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crafting", SaveGame)
	float AccumulatedSeconds = 0.f;

	/** Heure serveur du dernier démarrage de la progression ; négatif = en pause. Non sauvegardé (relatif à la session). */
	UPROPERTY(BlueprintReadOnly, Category = "Crafting")
	double RunningSinceServerTime = -1.0;
};

// ------------------------------------------------------------------------------------------------
// Sauvegarde (champs SaveGame, défauts neutres ; format v3 géré par Dev B)
// ------------------------------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBEquipmentSaveData
{
	GENERATED_BODY()

	/** Contenu du sac d'objets. */
	UPROPERTY(SaveGame)
	TArray<FEMBItemInstance> BagItems;

	/** Objets équipés (l'emplacement est redéduit de la ligne DT_Items à l'import). */
	UPROPERTY(SaveGame)
	TArray<FEMBItemInstance> EquippedItems;
};

USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBCraftingSaveData
{
	GENERATED_BODY()

	/** File de fabrication (les ingrédients sont déjà consommés). */
	UPROPERTY(SaveGame)
	TArray<FEMBCraftQueueEntry> Entries;
};
