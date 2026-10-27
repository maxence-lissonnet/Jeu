// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Misc/Guid.h"
#include "EMBItemTypes.h" // EEMBToolType (Dev F)
#include "EMBResourceNodeTypes.generated.h"

class AActor;
class AEMBFallingTree;
class AEMBResourceNode;

/**
 * Constantes de la récolte. Source : docs/00_Constantes_et_Contrats.md §13.
 * Les valeurs marquées (*) ne figurent pas au référentiel : ce sont des choix d'implémentation à équilibrer en playtest.
 */
namespace EMBHarvestDefaults
{
	// --- Référentiel §13 ---
	/** Distance serveur maximale (cm) entre l'instigateur et le nœud. */
	constexpr float MaxHarvestDistance = 400.f;
	/** Rendement d'un outil de tier >= MinToolTier mais < FullYieldTier. */
	constexpr float UnderTierYieldMultiplier = 0.3f;
	/** Craquement d'un arbre épuisé avant sa chute (s). */
	constexpr float FellingCrackSeconds = 2.f;
	constexpr float FallingTreeDamage = 150.f;
	constexpr float FallingTreePoise = 60.f;
	/** Durée de vie d'un arbre qui tombe (s) : sommeil physique puis disparition. */
	constexpr float FallingTreeLifetimeSeconds = 10.f;
	constexpr int32 MaxFallingTreesPerPlayer = 3;
	constexpr int32 MaxFallingTreesTotal = 12;
	/** Durée de vie des débris d'une fracture de rocher (s). */
	constexpr float FractureDebrisSeconds = 15.f;
	/** Un joueur ou une pièce plus proche (cm) que ce rayon reporte la réapparition. */
	constexpr float RespawnBlockRadius = 300.f;
	constexpr float RespawnDeferSeconds = 30.f;
	/** Durée de vie d'une carcasse dynamique (s). */
	constexpr float DynamicNodeLifeSpanSeconds = 600.f;
	/** (*) Contrôle de repli du jour/nuit des filons nocturnes (s) ; le basculement normal vient de OnEnvironmentChanged. */
	constexpr float NightFallbackRefreshSeconds = 60.f;
	/** (*) Bornes de ImportSaveData : nombre d'entrées maximum et secondes restantes maximum (24 h). */
	constexpr int32 MaxSavedNodeEntries = 100000;
	constexpr float MaxSavedRemainingSeconds = 86400.f;
	/** (*) Demi-côté (cm) de la boîte de collision de repli d'un nœud sans collision propre. */
	constexpr float FallbackHitBoxHalfExtent = 60.f;

	// --- (*) choix d'implémentation ---
	/** (*) Période de balayage des réapparitions (s) : un seul timer pour tous les nœuds épuisés. */
	constexpr float RespawnPollSeconds = 1.f;
	/** (*) Nombre maximal de carcasses dynamiques vivantes ; au-delà, la plus ancienne disparaît. */
	constexpr int32 MaxDynamicNodes = 64;
	/** (*) Délai entre l'épuisement d'une carcasse dynamique et sa disparition (s). */
	constexpr float DynamicDespawnDelaySeconds = 3.f;
	/** (*) Mise en sommeil physique de l'arbre tombé (s après le début de la chute). */
	constexpr float FallingTreeSleepSeconds = 5.f;
	/** (*) Fenêtre pendant laquelle l'arbre qui tombe blesse les pawns (s). */
	constexpr float FallingTreeDamageWindowSeconds = 4.f;
	/** (*) Vitesse minimale (cm/s) de l'arbre pour blesser un pawn au contact. */
	constexpr float FallingTreeMinHitSpeed = 100.f;
	/** (*) Vitesse communiquée au sommet de l'arbre pour amorcer la chute (cm/s). */
	constexpr float FallingTreeImpulseSpeed = 350.f;
	/** (*) Masse imposée à l'arbre simulé (kg), indépendante du volume du mesh. */
	constexpr float FallingTreeMassKg = 800.f;
	/** (*) Distance de pertinence réseau d'un nœud (cm). */
	constexpr float NodeNetCullDistance = 15000.f;

	/** Multiplicateur de rendement par tier d'outil (§13) : T1 x1,0 ; T2 x1,6 ; T3 x2,2 ; T4 x3,0 ; mains nues x1,0. */
	inline float GetTierYieldMultiplier(const int32 Tier)
	{
		if (Tier >= 4)
		{
			return 3.0f;
		}
		if (Tier == 3)
		{
			return 2.2f;
		}
		if (Tier == 2)
		{
			return 1.6f;
		}
		return 1.0f;
	}
}

/** Famille d'un nœud de ressource : détermine l'effet d'épuisement (chute, fracture) et le rendu. */
UENUM(BlueprintType)
enum class EEMBResourceNodeKind : uint8
{
	Tree,
	Rock,
	Vein,
	Bush,
	Carcass,
	Water
};

/** Raison pour laquelle un coup de récolte serait refusé (retour d'interface, calculable côté client). */
UENUM(BlueprintType)
enum class EEMBHarvestReject : uint8
{
	None,
	/** Nœud épuisé, en train de tomber, ou instigateur invalide. */
	Unavailable,
	/** Filon visible seulement la nuit, et c'est le jour. */
	NightOnly,
	/** Type d'outil inadapté. */
	WrongTool,
	/** Tier de l'outil insuffisant. */
	TierTooLow,
	/** Hors de portée (400 cm). */
	TooFar,
	/** Données du nœud indisponibles. */
	Unknown
};

/** Une ressource produite par un nœud : identifiant de l'inventaire et quantité de base (avant multiplicateurs). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBResourceYield
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	FName ResourceId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "0"))
	int32 Amount = 0;
};

/** Ligne de DT_ResourceNodes (Data/DT_ResourceNodes.csv). Valeurs : docs/00_Constantes_et_Contrats.md §13. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBResourceNodeRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	EEMBResourceNodeKind Kind = EEMBResourceNodeKind::Tree;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "1"))
	float MaxHealth = 100.f;

	/** None = mains nues (tout outil est accepté). Sinon le type d'outil doit correspondre exactement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	EEMBToolType RequiredToolType = EEMBToolType::None;

	/** Tier minimal de l'outil : en dessous, le coup est ignoré. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "0"))
	int32 MinToolTier = 0;

	/** Tier à partir duquel le rendement est complet ; entre MinToolTier et ce tier : x0,3. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "0"))
	int32 FullYieldTier = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	TArray<FEMBResourceYield> Yields;

	/** Secondes avant réapparition (0 = ne réapparaît pas). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "0"))
	float RespawnSeconds = 0.f;

	/** XP versée au joueur qui épuise le nœud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", meta = (ClampMin = "0"))
	int32 XPOnDepleted = 0;

	/** Visible et récoltable seulement la nuit (argent-lune). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	bool bNightOnly = false;

	/** Ne s'épuise jamais (source d'eau) : chaque coup valide verse le rendement de base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	bool bInexhaustible = false;

	/** Rocher : classe d'acteur de débris (GeometryCollection), chargée à la demande. Vide = disparition simple. Ne doit pas être répliquée. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	TSoftClassPtr<AActor> FractureClass;

	/** Arbre : classe de l'acteur de chute (sous-classe de AEMBFallingTree). Vide = AEMBFallingTree. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	TSoftClassPtr<AEMBFallingTree> FallingClass;

	/**
	 * Ajout (hors référentiel) : classe Blueprint du nœud, utilisée par SpawnDynamicNode pour porter le mesh de la
	 * carcasse. Vide = AEMBResourceNode (sans mesh). Ignorée pour les nœuds posés dans le niveau.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	TSoftClassPtr<AEMBResourceNode> NodeClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest")
	FText Description;
};

/** Un nœud posé épuisé : identifiant stable et secondes restantes avant réapparition. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBResourceNodeTimer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", SaveGame)
	FGuid NodeId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", SaveGame)
	float RemainingSeconds = 0.f;
};

/** Sauvegarde des nœuds : uniquement les nœuds posés épuisés (les carcasses dynamiques ne sont pas sauvegardées). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBResourceNodesSaveData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Harvest", SaveGame)
	TArray<FEMBResourceNodeTimer> DepletedNodes;
};
