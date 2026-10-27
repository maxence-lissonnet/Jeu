// Emberfall - types de données de la sauvegarde (Dev B, référentiel section 8).

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/SoftObjectPath.h"
#include "EMBBuildingTypes.h"
#include "EMBInventoryComponent.h"
#include "EMBProgressionTypes.h"
#include "EMBSurvivalTypes.h"
#include "EMBSaveTypes.generated.h"

/**
 * Remarque générale : UGameplayStatics::SaveGameToSlot ne sérialise que les propriétés marquées SaveGame. Tous les
 * champs ci-dessous le sont, y compris dans les structures imbriquées. La progression (structure de Dev A) est
 * incluse telle quelle : ses champs sont marqués SaveGame.
 */

/** Une pièce de construction sauvegardée. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBPieceSaveData
{
	GENERATED_BODY()

	/** Identifiant propre à cette sauvegarde (1..N), cible des liens de support des autres pièces. */
	UPROPERTY(SaveGame)
	int32 SaveId = INDEX_NONE;

	/** Ligne de DT_BuildPieces. */
	UPROPERTY(SaveGame)
	FName RowName = NAME_None;

	/** Chemin de la classe de la pièce (le Blueprint est rechargé au chargement). */
	UPROPERTY(SaveGame)
	FSoftClassPath PieceClassPath;

	UPROPERTY(SaveGame)
	FTransform Transform = FTransform::Identity;

	UPROPERTY(SaveGame)
	float Health = 0.f;

	UPROPERTY(SaveGame)
	EEMBBuildMaterial Material = EEMBBuildMaterial::Wood;

	UPROPERTY(SaveGame)
	EEMBPieceCategory Category = EEMBPieceCategory::Wall;

	/** Ancrée au sol (fondation, mobilier posé sur le terrain). */
	UPROPERTY(SaveGame)
	bool bAnchoredToGround = false;

	/** Identifiant stable du poseur (vide si inconnu). */
	UPROPERTY(SaveGame)
	FString PlacerStableId;

	/** SaveId des pièces voisines en contact structurel : sert uniquement à CONTRÔLER le recalcul, jamais à construire le graphe. */
	UPROPERTY(SaveGame)
	TArray<int32> SupportLinkIds;
};

/** Un joueur sauvegardé, clé = identifiant stable. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBPlayerSaveData
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FString StableId;

	/**
	 * Dernier instant (UTC) où le joueur a été capturé. Sert à départager les joueurs quand la sauvegarde dépasse la
	 * limite de 256 entrées : on garde les connectés, puis les plus récemment vus.
	 */
	UPROPERTY(SaveGame)
	FDateTime LastSeen = FDateTime(0);

	/**
	 * Progression du joueur. Les champs de FEMBProgressionSaveData et FEMBSkillRank sont marqués SaveGame par Dev A, donc
	 * sérialisés directement. Le format v1 n'ayant jamais été publié, aucune migration n'est nécessaire (l'ancienne
	 * version avec blob d'octets n'a jamais existé hors développement).
	 */
	UPROPERTY(SaveGame)
	FEMBProgressionSaveData Progression;

	/**
	 * Vrai si l'état du pawn (inventaire, PV, endurance, positions) a été réellement capturé au moins une fois. Faux :
	 * seule la progression est connue, la partie pawn ne doit PAS être restaurée (elle mettrait PV et endurance à 0).
	 */
	UPROPERTY(SaveGame)
	bool bHasPawnState = false;

	/** (v2) Jauges de survie (faim, soif, Corruption...). Valable seulement si bHasSurvivalState. */
	UPROPERTY(SaveGame)
	FEMBSurvivalSaveData Survival;

	/** (v2) Faux pour une sauvegarde v1 migrée : le joueur démarre avec les valeurs de survie par défaut. */
	UPROPERTY(SaveGame)
	bool bHasSurvivalState = false;

	/**
	 * (v2) SaveId (dans CE fichier) de la pièce à laquelle le joueur est lié comme point de réapparition, INDEX_NONE
	 * si aucune. Les SaveId sont renumérotés à chaque sauvegarde : la valeur n'a de sens que dans le fichier qui la contient.
	 */
	UPROPERTY(SaveGame)
	int32 BoundRespawnPieceSaveId = INDEX_NONE;

	UPROPERTY(SaveGame)
	TArray<FEMBResourceEntry> Resources;

	UPROPERTY(SaveGame)
	float Health = 0.f;

	UPROPERTY(SaveGame)
	float Stamina = 0.f;

	/** Vrai si PawnTransform est exploitable. Faux : le joueur apparaît au PlayerStart (mort sans point sûr connu, ou jamais vu). */
	UPROPERTY(SaveGame)
	bool bHasPawnTransform = false;

	UPROPERTY(SaveGame)
	FTransform PawnTransform = FTransform::Identity;

	/** Vrai si LastSafeTransform est connu (dernière position vivante hors combat, relevée toutes les 10 s). */
	UPROPERTY(SaveGame)
	bool bHasLastSafeTransform = false;

	UPROPERTY(SaveGame)
	FTransform LastSafeTransform = FTransform::Identity;
};

/** (v2) Un sac de mort sauvegardé. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBDeathBagSaveData
{
	GENERATED_BODY()

	/** Identifiant stable du propriétaire. Seul lui peut ramasser le sac. */
	UPROPERTY(SaveGame)
	FString OwnerStableId;

	UPROPERTY(SaveGame)
	FTransform Transform = FTransform::Identity;

	UPROPERTY(SaveGame)
	TArray<FEMBResourceEntry> Contents;
};

/** État du monde (hors constructions et joueurs). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBWorldSaveData
{
	GENERATED_BODY()

	/**
	 * Événements MONDE acquis (Event.Boss.Killed.*) : un boss vaincu ne réapparaît pas. Les lieux découverts
	 * (Event.Location.Discovered.*) sont des événements propres à chaque joueur : ils sont enregistrés dans sa
	 * progression (ObtainedTriggers), pas ici.
	 */
	UPROPERTY(SaveGame)
	FGameplayTagContainer WorldEvents;

	/** (v2) Sacs de mort non encore ramassés (un par joueur au plus). */
	UPROPERTY(SaveGame)
	TArray<FEMBDeathBagSaveData> DeathBags;
};
