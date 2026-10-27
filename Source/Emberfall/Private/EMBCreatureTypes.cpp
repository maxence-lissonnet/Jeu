// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCreatureTypes.h"

#include "Emberfall.h"

#include "Engine/DataTable.h"

namespace EMBCreatureTags
{
	// --- Goule de cendre ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Ghoul_Claw, "Attack.Ghoul.Claw", "Griffade de la goule de cendre (parable).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Ghoul_Lunge, "Attack.Ghoul.Lunge", "Charge maladroite de la goule (bond).");

	// --- Wyverneau des rocs ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Wyvern_Bite, "Attack.Wyvern.Bite", "Morsure du wyverneau (parable).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Wyvern_LowSwoop, "Attack.Wyvern.LowSwoop", "Piqué bas du wyverneau.");

	// --- Sylvadrac ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Sylvadrac_Bite, "Attack.Sylvadrac.Bite", "Morsure du Sylvadrac (parable, empoisonne).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Sylvadrac_LowDive, "Attack.Sylvadrac.LowDive", "Piqué bas du Sylvadrac.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Sylvadrac_PoisonCloud, "Attack.Sylvadrac.PoisonCloud", "Nuage toxique persistant (8 s).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Sylvadrac_Roar, "Attack.Sylvadrac.Roar", "Rugissement d'avertissement à l'entrée du territoire.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Sylvadrac_Poison, "Attack.Sylvadrac.Poison", "Poison : dégâts continus (20/s).");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Creature_Killed_Sylvadrac, "Event.Creature.Killed.Sylvadrac", "Sylvadrac abattu (déclencheur joueur, jamais un événement monde : le dragon réapparaît).");
}

namespace EMBCreatureData
{
	bool FindRow(const TSoftObjectPtr<UDataTable>& Table, const FName RowName, FEMBCreatureRow& OutRow)
	{
		if (Table.IsNull() || RowName.IsNone())
		{
			return false;
		}

		const UDataTable* Loaded = Table.LoadSynchronous();
		if (!Loaded)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Bestiaire : table %s introuvable."), *Table.ToString());
			return false;
		}
		if (Loaded->GetRowStruct() != FEMBCreatureRow::StaticStruct())
		{
			UE_LOG(LogEmberfall, Error, TEXT("Bestiaire : %s n'utilise pas la structure FEMBCreatureRow, table ignoree."), *GetNameSafe(Loaded));
			return false;
		}

		const FEMBCreatureRow* Row = Loaded->FindRow<FEMBCreatureRow>(RowName, TEXT("EMBCreatureData::FindRow"), false);
		if (!Row)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Bestiaire : ligne '%s' absente de %s."), *RowName.ToString(), *GetNameSafe(Loaded));
			return false;
		}

		OutRow = *Row;
		return true;
	}

	FSoftObjectPath GetDefaultTablePath()
	{
		return FSoftObjectPath(TEXT("/Game/Data/DT_Creatures.DT_Creatures"));
	}
}
