// Emberfall - objet de sauvegarde (Dev B, référentiel section 8).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "EMBSaveTypes.h"
#include "EMBSaveGame.generated.h"

/** Résultat de la mise à niveau d'un fichier de sauvegarde. */
enum class EEMBMigrationResult : uint8
{
	/** Fichier à la version courante (migré si nécessaire) : utilisable. */
	Ok,
	/** Fichier plus récent que ce binaire : illisible ici mais PRÉCIEUX, aucune écriture ne doit le détruire. */
	TooRecent,
	/** Version < 1 ou migration impossible : fichier inutilisable, ignoré sans bloquer les écritures. */
	Invalid
};

/**
 * Contenu d'un slot de sauvegarde (un fichier = _A ou _B d'un monde).
 *
 * Évolution du format : incrémenter CurrentFormatVersion, ajouter un cas dans MigrateStep (migration N -> N+1) et ne
 * JAMAIS réutiliser ni renommer une propriété existante (le système de propriétés étiquetées d'Unreal tolère l'ajout
 * de champs : ils prennent leur valeur par défaut dans les anciens fichiers).
 */
UCLASS()
class EMBERFALL_API UEMBSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/**
	 * Version du format actuellement écrite. Historique : 1 = format initial ; 2 = survie par joueur, liaison de
	 * réapparition, sacs de mort (migration 1 -> 2 : champs ajoutés, valeurs par défaut).
	 */
	static constexpr int32 CurrentFormatVersion = 2;

	/**
	 * Version du format du fichier. Valeur par défaut VOLONTAIREMENT 0 (invalide), renseignée explicitement à l'écriture.
	 * Piège : la sérialisation par propriétés étiquetées n'écrit pas une valeur identique à celle du CDO. Avec un défaut
	 * de 1, un fichier de version 1 n'écrirait pas ce champ, et un fichier vide ou tronqué se relirait « version 1 »
	 * valide. Avec 0, la version 1 est écrite pour de bon, et un fichier sans ce champ est rejeté (MigrateToCurrent
	 * refuse toute version < 1). Même raison pour bComplete (défaut false) et les décomptes (défaut -1).
	 */
	UPROPERTY(SaveGame)
	int32 SaveFormatVersion = 0;

	/** Instant d'écriture (UTC). Sert à choisir le plus récent des slots _A / _B. */
	UPROPERTY(SaveGame)
	FDateTime SaveTimestamp = FDateTime(0);

	/** Monde (0..2) auquel appartient la sauvegarde. */
	UPROPERTY(SaveGame)
	int32 SlotIndex = 0;

	UPROPERTY(SaveGame)
	FEMBWorldSaveData World;

	UPROPERTY(SaveGame)
	TArray<FEMBPlayerSaveData> Players;

	UPROPERTY(SaveGame)
	TArray<FEMBPieceSaveData> Pieces;

	// ---- Contrôle d'intégrité : ces trois champs doivent rester les DERNIERS déclarés. Les propriétés étant écrites
	// ---- dans l'ordre de déclaration, un fichier tronqué (écriture interrompue) perd d'abord ceux-ci : bComplete reste
	// ---- à false (valeur par défaut) ou les décomptes ne correspondent pas, et le fichier est rejeté au chargement.

	UPROPERTY(SaveGame)
	int32 ExpectedPlayerCount = -1;

	UPROPERTY(SaveGame)
	int32 ExpectedPieceCount = -1;

	/** Vrai uniquement si l'écriture est allée jusqu'au bout (positionné juste avant l'envoi à AsyncSaveGameToSlot). */
	UPROPERTY(SaveGame)
	bool bComplete = false;

	/** Renseigne les trois champs de contrôle à partir du contenu. À appeler une fois la sauvegarde remplie. */
	void FinalizeForWrite();

	/** Vrai si bComplete est positionné et si les décomptes correspondent au contenu relu. */
	bool IsComplete() const;

	/**
	 * Amène la sauvegarde à CurrentFormatVersion par étapes successives N -> N+1.
	 * @return Ok si utilisable ; TooRecent si la sauvegarde vient d'un binaire plus récent (à protéger : bloquer les
	 *         écritures) ; Invalid si la version est inférieure à 1 ou si une étape échoue (à ignorer simplement).
	 */
	EEMBMigrationResult MigrateToCurrent();

	/** Vrai si cette sauvegarde est plus récente que l'autre (horodatage ; à égalité, version de format supérieure). */
	bool IsNewerThan(const UEMBSaveGame* Other) const;

private:
	/** Migration de FromVersion vers FromVersion + 1. */
	bool MigrateStep(int32 FromVersion);
};
