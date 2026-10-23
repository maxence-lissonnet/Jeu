// Emberfall - types partagés du système de construction (Dev B).
// Source des valeurs : docs/00_Constantes_et_Contrats.md, section 5.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "EMBBuildingTypes.generated.h"

class AEMBBuildablePiece;
class UTexture2D;

/** Matériau d'une pièce : détermine PV et décroissance d'intégrité (section 5). */
UENUM(BlueprintType)
enum class EEMBBuildMaterial : uint8
{
	Wood      UMETA(DisplayName = "Wood"),
	Stone     UMETA(DisplayName = "Stone"),
	Iron      UMETA(DisplayName = "Iron"),
	Draconic  UMETA(DisplayName = "Draconic")
};

/** Catégorie d'une pièce (miroir des tags Building.Category.*). */
UENUM(BlueprintType)
enum class EEMBPieceCategory : uint8
{
	Foundation  UMETA(DisplayName = "Foundation"),
	Floor       UMETA(DisplayName = "Floor"),
	Wall        UMETA(DisplayName = "Wall"),
	Roof        UMETA(DisplayName = "Roof"),
	Pillar      UMETA(DisplayName = "Pillar"),
	Stairs      UMETA(DisplayName = "Stairs"),
	Furniture   UMETA(DisplayName = "Furniture"),
	Workstation UMETA(DisplayName = "Workstation")
};

/** Résultat d'une vérification de placement (aperçu local ET validation serveur). */
UENUM(BlueprintType)
enum class EEMBPlacementResult : uint8
{
	Valid                 UMETA(DisplayName = "Valid"),
	TooFar                UMETA(DisplayName = "Too Far"),
	Overlapping           UMETA(DisplayName = "Overlapping"),
	NoSupport             UMETA(DisplayName = "No Support"),
	SlopeTooSteep         UMETA(DisplayName = "Slope Too Steep"),
	NotEnoughResources    UMETA(DisplayName = "Not Enough Resources"),
	NoBuildZone           UMETA(DisplayName = "No Build Zone"),
	InsufficientIntegrity UMETA(DisplayName = "Insufficient Integrity"),
	InvalidPiece          UMETA(DisplayName = "Invalid Piece"),
	InvalidTransform      UMETA(DisplayName = "Invalid Transform"),
	RateLimited           UMETA(DisplayName = "Rate Limited"),
	/** Ajouté en fin d'enum (v1.1) pour ne pas décaler les valeurs existantes : 3 000 pièces/base ou 15 000/monde. */
	LimitReached          UMETA(DisplayName = "Limit Reached")
};

/**
 * Constantes de la section 5 du référentiel. Valeurs reprises à l'identique.
 * Les composants qui doivent être réglables en Blueprint les recopient dans des UPROPERTY(EditDefaultsOnly).
 */
namespace EMBBuildingDefaults
{
	constexpr float GridSize = 300.f;                       // cm, fondation 300x300x50
	constexpr float WallHeight = 300.f;                     // cm
	constexpr float MaxPlacementDistance = 800.f;           // cm, depuis le personnage
	constexpr float MaxFoundationSlopeDeg = 30.f;           // degrés
	constexpr float GroundTraceDepth = 180.f;               // cm (v1.1) : 300 cm sur pente de 30 degrés => 173 cm d'enfoncement
	constexpr float GridStepLarge = 300.f;                  // fondation, sol, toit
	constexpr float GridStepMedium = 150.f;                 // mur, pilier, escalier
	constexpr float GridStepSmall = 25.f;                   // mobilier, poste de travail
	constexpr float GridPositionTolerance = 5.f;            // cm, validation serveur de l'alignement
	constexpr int32 MaxPiecesPerBase = 3000;                // composante connexe
	constexpr int32 MaxPiecesPerWorld = 15000;
	constexpr float ServerDistanceTolerance = 150.f;        // cm, latence : 950 cm effectifs côté serveur
	constexpr float PreplacedGracePeriod = 1.0f;            // s avant la 1re évaluation d'intégrité (pièces pré-placées / streamées)
	constexpr float MinContactSizeDefault = 10.f;           // cm, recouvrement minimal pour un contact par une face
	constexpr double MaxWorldCoordinate = 2097152.0;        // cm, limite du monde UE : borne des coordonnées acceptées (RPC, sauvegarde)
	constexpr float SocketSnapRadius = 150.f;               // cm
	constexpr float OverlapBoxShrink = 0.95f;               // boîte réduite de 5 % pour tolérer les contacts
	constexpr float RotationStepSnapped = 90.f;             // degrés
	constexpr float RotationStepFree = 15.f;                // degrés (mobilier)
	constexpr float FurnitureSnap = 25.f;                   // cm
	constexpr float GroundedIntegrity = 100.f;
	constexpr float CollapseThreshold = 0.f;                // intégrité <= 0 => effondrement
	constexpr float CollapseWaveDelay = 0.08f;              // s entre deux niveaux de propagation
	constexpr int32 MaxCollapsePerFrame = 32;
	constexpr float PillarVerticalDecayMultiplier = 0.5f;
	constexpr float VerticalDeltaThreshold = 50.f;          // cm : liaison verticale si Δz > 50
	constexpr float PreviewGreenAbove = 50.f;               // vert > 50
	constexpr float PreviewYellowFrom = 25.f;               // jaune 25-50, orange 1-24
}

/** Statistiques d'un matériau (section 5). */
USTRUCT(BlueprintType)
struct FEMBMaterialStats
{
	GENERATED_BODY()

	/** Points de vie d'une pièce de ce matériau. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	float MaxHealth = 500.f;

	/** Perte d'intégrité quand la pièce est AU-DESSUS de son support (Δz > 50 cm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	float VerticalDecay = 10.f;

	/** Perte d'intégrité pour une liaison horizontale / en porte-à-faux. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	float HorizontalDecay = 25.f;

	FEMBMaterialStats() = default;
	FEMBMaterialStats(float InMaxHealth, float InVerticalDecay, float InHorizontalDecay)
		: MaxHealth(InMaxHealth), VerticalDecay(InVerticalDecay), HorizontalDecay(InHorizontalDecay)
	{
	}
};

namespace EMBBuildingDefaults
{
	/** Table statique fidèle à la section 5 (bois T1, pierre T2, fer forgé T3, draconique T4). */
	FORCEINLINE FEMBMaterialStats GetMaterialStats(EEMBBuildMaterial Material)
	{
		switch (Material)
		{
		case EEMBBuildMaterial::Wood:     return FEMBMaterialStats(500.f, 10.f, 25.f);
		case EEMBBuildMaterial::Stone:    return FEMBMaterialStats(1500.f, 6.f, 20.f);
		case EEMBBuildMaterial::Iron:     return FEMBMaterialStats(3000.f, 5.f, 12.f);
		case EEMBBuildMaterial::Draconic: return FEMBMaterialStats(6000.f, 3.f, 8.f);
		default:                          return FEMBMaterialStats();
		}
	}
}

/** Point d'accrochage (socket "Snap_*") d'une pièce, en espace local ou monde selon le contexte. */
USTRUCT(BlueprintType)
struct FEMBSnapSocket
{
	GENERATED_BODY()

	/** Convention de nommage : Snap_<Kind>_<Suffixe>. Deux sockets sont compatibles si <Kind> est identique. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Building")
	FTransform Transform = FTransform::Identity;

	/** <Kind> du nom, précalculé une fois par la pièce (évite de parser des chaînes à 30 Hz). Renseigné par GetSnapSocketsLocal. */
	UPROPERTY(Transient)
	FName Kind = NAME_None;
};

/** Tableau de sockets sans allocation dynamique dans le cas courant (une pièce a quelques sockets). */
using FEMBSnapSocketArray = TArray<FEMBSnapSocket, TInlineAllocator<8>>;

/** Ligne de la DataTable DT_BuildPieces. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBBuildPieceRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	EEMBPieceCategory Category = EEMBPieceCategory::Foundation;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	EEMBBuildMaterial Material = EEMBBuildMaterial::Wood;

	/** Classe de la pièce (Blueprint dérivé de AEMBBuildablePiece). Soft pour ne pas tout charger au démarrage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	TSoftClassPtr<AEMBBuildablePiece> PieceClass;

	/** Coût en ressources (identifiants FName de la section 3.5). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	TMap<FName, int32> Cost;

	/** Vrai : la pièce doit reposer sur une surface (terrain ou pièce) sous son pivot. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	bool bRequiresGroundSupport = false;

	/** Vrai (fondations) : ancrée au sol, intégrité fixée à GroundedIntegrity (100). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	bool bIsGroundAnchor = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	TSoftObjectPtr<UTexture2D> Icon;

	/**
	 * Glyphe requis pour construire cette pièce (progression). Vide = toujours disponible.
	 * Colonne ajoutée EN FIN de ligne (v1.2) : les CSV existants restent importables.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Building")
	FName RequiredGlyph = NAME_None;

	/** Mobilier et postes d'artisanat : rotation par 15 degrés, jamais porteurs. */
	bool IsFurnitureLike() const
	{
		return Category == EEMBPieceCategory::Furniture || Category == EEMBPieceCategory::Workstation;
	}

	float GetRotationStep() const
	{
		return IsFurnitureLike() ? EMBBuildingDefaults::RotationStepFree : EMBBuildingDefaults::RotationStepSnapped;
	}
};

/** Résultat complet d'une évaluation de placement (aperçu local ou validation serveur). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBPlacementEvaluation
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Building")
	EEMBPlacementResult Result = EEMBPlacementResult::InvalidPiece;

	/** Transform final (après accrochage au sol en mode aperçu). */
	UPROPERTY(BlueprintReadOnly, Category = "Building")
	FTransform Transform = FTransform::Identity;

	/** Intégrité prédite de la pièce (0..100). */
	UPROPERTY(BlueprintReadOnly, Category = "Building")
	float PredictedIntegrity = 0.f;

	/** Vrai si la pièce est ancrée au terrain (fondation, ou mobilier posé sur le terrain). */
	UPROPERTY(BlueprintReadOnly, Category = "Building")
	bool bAnchoredToGround = false;
};
