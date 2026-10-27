// Emberfall - objet de sauvegarde (Dev B, référentiel section 8).

#include "EMBSaveGame.h"

#include "Emberfall.h"

EEMBMigrationResult UEMBSaveGame::MigrateToCurrent()
{
	if (SaveFormatVersion > CurrentFormatVersion)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde de version %d plus recente que ce binaire (version %d)."), SaveFormatVersion, CurrentFormatVersion);
		return EEMBMigrationResult::TooRecent;
	}
	if (SaveFormatVersion < 1)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde de version %d invalide : ignoree."), SaveFormatVersion);
		return EEMBMigrationResult::Invalid;
	}

	while (SaveFormatVersion < CurrentFormatVersion)
	{
		if (!MigrateStep(SaveFormatVersion))
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Echec de la migration de la sauvegarde de la version %d vers %d."), SaveFormatVersion, SaveFormatVersion + 1);
			return EEMBMigrationResult::Invalid;
		}
		++SaveFormatVersion;
	}
	return EEMBMigrationResult::Ok;
}

bool UEMBSaveGame::MigrateStep(int32 FromVersion)
{
	switch (FromVersion)
	{
	case 1:
		// 1 -> 2 : la version 2 n'a fait qu'AJOUTER des champs (survie, liaison de réapparition, sacs de mort). Le système
		// de propriétés étiquetées les laisse à leur valeur par défaut à la lecture d'un fichier v1 :
		//  - bHasSurvivalState = false : les joueurs reprennent les valeurs de survie par défaut ;
		//  - BoundRespawnPieceSaveId = INDEX_NONE : aucune liaison (PlayerStart) ;
		//  - DeathBags vide : aucun sac.
		// Rien à convertir.
		return true;
	case 2:
		// 2 -> 3 : AJOUT de champs seulement (équipement, file de fabrication, nœuds de récolte, météo). Un fichier v2 les
		// lit à leurs valeurs par défaut, avec tous les drapeaux bHas...State à faux : à l'import, rien n'est appliqué,
		// et les systèmes concernés démarrent à neuf (aucun objet, nœuds pleins, météo et heure initiales).
		// Les futures étapes s'ajoutent ici (case 3: ...).
		return true;
	default:
		return false;
	}
}

void UEMBSaveGame::FinalizeForWrite()
{
	ExpectedPlayerCount = Players.Num();
	ExpectedPieceCount = Pieces.Num();
	bComplete = true;
}

bool UEMBSaveGame::IsComplete() const
{
	return bComplete && ExpectedPlayerCount == Players.Num() && ExpectedPieceCount == Pieces.Num();
}

bool UEMBSaveGame::IsNewerThan(const UEMBSaveGame* Other) const
{
	if (!Other)
	{
		return true;
	}
	if (SaveTimestamp != Other->SaveTimestamp)
	{
		return SaveTimestamp > Other->SaveTimestamp;
	}
	return SaveFormatVersion > Other->SaveFormatVersion;
}
