// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"

AEMBPlayerState::AEMBPlayerState()
{
	Progression = CreateDefaultSubobject<UEMBProgressionComponent>(TEXT("Progression"));

	// Le PlayerState se réplique à 1 Hz par défaut : trop lent pour un achat de glyphe ou un gain de niveau.
	// 10 Hz suffit (le composant force aussi un envoi à chaque changement) pour un coût réseau négligeable.
	SetNetUpdateFrequency(10.f);
}

void AEMBPlayerState::StorePendingLoadout(const FEMBEquipmentSaveData& Equipment, const FEMBCraftingSaveData& Crafting)
{
	PendingEquipment = Equipment;
	PendingCrafting = Crafting;
	bHasPendingLoadout = true;
}

bool AEMBPlayerState::ConsumePendingLoadout(FEMBEquipmentSaveData& OutEquipment, FEMBCraftingSaveData& OutCrafting)
{
	if (!bHasPendingLoadout)
	{
		return false;
	}
	OutEquipment = PendingEquipment;
	OutCrafting = PendingCrafting;
	PendingEquipment = FEMBEquipmentSaveData();
	PendingCrafting = FEMBCraftingSaveData();
	bHasPendingLoadout = false;
	return true;
}

void AEMBPlayerState::CopyProperties(APlayerState* NewPlayerState)
{
	Super::CopyProperties(NewPlayerState);

	// Export / import plutôt qu'une copie champ à champ : l'import assainit et réapplique les bonus.
	if (const AEMBPlayerState* NewState = Cast<AEMBPlayerState>(NewPlayerState))
	{
		if (Progression && NewState->Progression)
		{
			FEMBProgressionSaveData Data;
			Progression->ExportSaveData(Data);
			NewState->Progression->ImportSaveData(Data);
		}
	}
}

void AEMBPlayerState::OverrideWith(APlayerState* OldPlayerState)
{
	Super::OverrideWith(OldPlayerState);

	if (const AEMBPlayerState* OldState = Cast<AEMBPlayerState>(OldPlayerState))
	{
		if (Progression && OldState->Progression)
		{
			FEMBProgressionSaveData Data;
			OldState->Progression->ExportSaveData(Data);
			Progression->ImportSaveData(Data);
		}
	}
}
