// Emberfall - objet de sauvegarde (Dev B, référentiel section 8).

#include "EMBSaveGame.h"

#include "Emberfall.h"

bool UEMBSaveGame::MigrateToCurrent()
{
	if (SaveFormatVersion > CurrentFormatVersion)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde de version %d plus recente que ce binaire (version %d) : ignoree."), SaveFormatVersion, CurrentFormatVersion);
		return false;
	}
	if (SaveFormatVersion < 1)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde de version %d invalide : ignoree."), SaveFormatVersion);
		return false;
	}

	while (SaveFormatVersion < CurrentFormatVersion)
	{
		if (!MigrateStep(SaveFormatVersion))
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Echec de la migration de la sauvegarde de la version %d vers %d."), SaveFormatVersion, SaveFormatVersion + 1);
			return false;
		}
		++SaveFormatVersion;
	}
	return true;
}

bool UEMBSaveGame::MigrateStep(int32 FromVersion)
{
	switch (FromVersion)
	{
	// Version 1 = format initial : aucune migration à ce jour. Les futures étapes s'ajoutent ici :
	//   case 1: /* convertir 1 -> 2 */ return true;
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
