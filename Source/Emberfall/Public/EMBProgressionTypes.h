// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "EMBProgressionTypes.generated.h"

/** Arbres de glyphes (savoir-faire acquis par le joueur). */
UENUM(BlueprintType)
enum class EEMBGlyphTree : uint8
{
	Survival,
	Construction,
	Forge,
	Alchemy,
	Arcana
};

/** Branches de l'arbre de compétences. */
UENUM(BlueprintType)
enum class EEMBSkillBranch : uint8
{
	WeaponMastery,
	Survival,
	PrimordialMagic
};

/** Statistique modifiée par un passif du Skill Tree (référentiel 7.3). */
UENUM(BlueprintType)
enum class EEMBPassiveStat : uint8
{
	None,
	PhysicalDamage,
	AttackStaminaCost,
	RiposteMultiplier,
	DodgeIFrames,
	GuardStaminaCost,
	ParryWindowFrames,
	BuildCost,
	CarryWeight,
	HungerThirstRate,
	HarvestYield,
	ComfortRange,
	CorruptionGain,
	FireResistance
};

/** Comportement des glyphes secrets : le déclencheur les révèle (à acheter) ou les débloque gratuitement. */
UENUM(BlueprintType)
enum class EEMBSecretGlyphMode : uint8
{
	RevealOnly,
	AutoUnlock
};

/** Position sauvegardée d'un joueur mort : dernier point sûr, ou position telle quelle. */
UENUM(BlueprintType)
enum class EEMBDeathSaveBehavior : uint8
{
	RespawnAtSafePoint,
	KeepPosition
};

UENUM(BlueprintType)
enum class EEMBGlyphState : uint8
{
	Hidden,
	Locked,
	Available,
	Unlocked
};

/**
 * Somme des bonus de passifs d'un joueur. Un float par EEMBPassiveStat, additif : 0 = neutre.
 * Les unités sont celles de ValuePerRank (fractions pour les pourcentages, frames pour les fenêtres).
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBPassiveModifiers
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float PhysicalDamage = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float AttackStaminaCost = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float RiposteMultiplier = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float DodgeIFrames = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float GuardStaminaCost = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float ParryWindowFrames = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float BuildCost = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float CarryWeight = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float HungerThirstRate = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float HarvestYield = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float ComfortRange = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float CorruptionGain = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passives") float FireResistance = 0.f;

	/** Ajoute Value à la statistique donnée (None est ignoré). */
	void Add(EEMBPassiveStat Stat, float Value)
	{
		switch (Stat)
		{
		case EEMBPassiveStat::PhysicalDamage:     PhysicalDamage += Value; break;
		case EEMBPassiveStat::AttackStaminaCost:  AttackStaminaCost += Value; break;
		case EEMBPassiveStat::RiposteMultiplier:  RiposteMultiplier += Value; break;
		case EEMBPassiveStat::DodgeIFrames:       DodgeIFrames += Value; break;
		case EEMBPassiveStat::GuardStaminaCost:   GuardStaminaCost += Value; break;
		case EEMBPassiveStat::ParryWindowFrames:  ParryWindowFrames += Value; break;
		case EEMBPassiveStat::BuildCost:          BuildCost += Value; break;
		case EEMBPassiveStat::CarryWeight:        CarryWeight += Value; break;
		case EEMBPassiveStat::HungerThirstRate:   HungerThirstRate += Value; break;
		case EEMBPassiveStat::HarvestYield:       HarvestYield += Value; break;
		case EEMBPassiveStat::ComfortRange:       ComfortRange += Value; break;
		case EEMBPassiveStat::CorruptionGain:     CorruptionGain += Value; break;
		case EEMBPassiveStat::FireResistance:     FireResistance += Value; break;
		default: break;
		}
	}
};

/** Rang acquis dans un nœud du Skill Tree (tableau répliqué : pas de TMap). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBSkillRank
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Skill")
	FName NodeId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Skill")
	int32 Rank = 0;
};

/** Progression d'un joueur telle qu'elle est sauvegardée. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBProgressionSaveData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	// Défaut 0 volontairement : une valeur égale au défaut n'est pas écrite par la sérialisation par propriétés,
	// donc une sauvegarde de niveau 1 relue avec un défaut de 1 serait indiscernable d'un champ absent. L'import
	// ramène toujours le niveau à >= 1 et le recalcule depuis l'XP.
	int32 Level = 0;

	/** XP cumulée depuis le niveau 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	int64 TotalXP = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	int32 UnspentGlyphPoints = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	int32 UnspentAwakeningPoints = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	TArray<FName> UnlockedGlyphs;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	TArray<FName> RevealedGlyphs;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	TArray<FEMBSkillRank> SkillRanks;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	TArray<FName> Knowledge;

	/** Événements Event.* acquis par ce joueur. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Progression")
	FGameplayTagContainer ObtainedTriggers;
};

/**
 * Ligne de la DataTable des glyphes. Les noms de colonnes sont figés : le CSV du chef d'équipe
 * (Data/*.csv) doit s'importer tel quel avec ce type de ligne.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBGlyphRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	EEMBGlyphTree Tree = EEMBGlyphTree::Survival;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	int32 Tier = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	int32 Cost = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	int32 RequiredLevel = 1;

	/** Glyphes à posséder avant celui-ci (noms de ligne). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	TArray<FName> ParentGlyphs;

	/** Connaissances (objets, notes, lieux) à avoir découvertes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	TArray<FName> RequiredKnowledge;

	/** Événement (EMBTags::Event_*) qui révèle ou débloque ce glyphe secret. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	FGameplayTag SecretTrigger;

	/** Masqué dans l'arbre tant qu'il n'est pas révélé. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	bool bHidden = false;

	/** Tag accordé au joueur une fois le glyphe appris (verrou de recettes, bonus...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	FGameplayTag GrantedTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Glyph")
	FText Description;
};

/** Ligne de la DataTable de l'arbre de compétences. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBSkillNodeRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	EEMBSkillBranch Branch = EEMBSkillBranch::WeaponMastery;

	/** Vrai : compétence active (déclenchée) ; faux : bonus passif. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	bool bIsActive = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	int32 Cost = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	int32 RequiredLevel = 1;

	/** Nœuds à posséder avant celui-ci (noms de ligne). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	TArray<FName> ParentNodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	int32 MaxRank = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FGameplayTag GrantedTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	FText Description;

	// Colonnes ajoutées en fin de ligne (v1.2) : l'ordre du CSV suit celui-ci.

	/** Statistique modifiée par ce passif (None pour une compétence active). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	EEMBPassiveStat PassiveStat = EEMBPassiveStat::None;

	/** Bonus par rang (fraction ou frames selon la statistique). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	float ValuePerRank = 0.f;

	/** Savoirs à posséder pour acheter un rang (ex. Lore_DraconicTongue pour la Communion draconique). Colonne en fin de ligne. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Skill")
	TArray<FName> RequiredKnowledge;
};
