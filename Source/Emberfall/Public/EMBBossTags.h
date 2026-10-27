// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"

/**
 * Gameplay Tags natifs des attaques des boss 1 et 2 (référentiel §11). Même convention que EMBTags :
 * utilisation EMBBossTags::Attack_Guardian_HammerFist.GetTag(), jamais dans un constructeur de CDO (le
 * gestionnaire de tags peut ne pas être prêt) : résoudre à l'usage ou au BeginPlay.
 *
 * Les tags du dragon (Attack.Dragon.*) restent dans EMBTags (EMBGameplayTags.h, Dev A).
 */
namespace EMBBossTags
{
	// --- Colosse d'Ossivaire (Gardien) ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_HammerFist);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_ArmSweep);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_LionBite);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_SerpentLash);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_RamCharge);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_Quake);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_RuneWaves);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_LionRoar);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Guardian_Resonance);

	// --- Nyxariel, l'Aile Flétrie (Séraphe) ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_Thrust);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_Feint);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_WingScissors);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_FeatherRain);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_LanceThrow);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_SpiralDive);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_TwinShadows);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_BlackHalo);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Seraph_DeadStarFall);
}
