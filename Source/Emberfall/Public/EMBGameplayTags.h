// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"

/**
 * Gameplay Tags natifs d'Emberfall (contrat 3.7).
 *
 * Les tags sont enregistrés au chargement du module (avant tout asset) : ils sont donc
 * utilisables sans fichier .ini. Utilisation : EMBTags::State_Dead.GetTag().
 *
 * Attention : ne pas appeler GetTag() dans un constructeur de CDO (le gestionnaire de tags
 * peut ne pas être prêt) ; résoudre les tags au BeginPlay ou à l'usage.
 */
namespace EMBTags
{
	// --- États de combat ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Combat_Invulnerable);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Combat_Staggered);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Combat_HyperArmor);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);

	// --- Attaques du joueur ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Player_Light);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Player_Heavy);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Player_Charged);

	// --- Attaques du dragon ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_Bite);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_ClawSwipe);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_TailSweep);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_WingBuffet);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_BreathCone);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_BreathSweep);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_Dive);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_AerialStrafe);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_EmberRain);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_Grab);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Dragon_Cataclysm);

	// --- Phases de boss ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Boss_Phase_1);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Boss_Phase_2);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Boss_Phase_3);

	// --- Construction : matériaux ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Material_Wood);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Material_Stone);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Material_Iron);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Material_Draconic);

	// --- Construction : catégories ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Foundation);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Floor);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Wall);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Roof);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Pillar);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Stairs);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Furniture);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_Category_Workstation);

	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Building_NoBuildZone);

	// --- Événements de progression (déclencheurs de glyphes secrets, quêtes) ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_Killed_Guardian);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_Killed_Seraph);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Boss_Killed_Dragon);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Location_Discovered_SunkenLibrary);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Location_Discovered_AshAltar);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Location_Discovered_StarWell);
}
