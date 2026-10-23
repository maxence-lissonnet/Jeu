// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBGameplayTags.h"

namespace EMBTags
{
	// --- États de combat ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Combat_Invulnerable, "State.Combat.Invulnerable", "I-frames : aucun coup ne touche (roulade).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Combat_Staggered, "State.Combat.Staggered", "Étourdi : poise brisée ou garde rompue.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Combat_HyperArmor, "State.Combat.HyperArmor", "Hyper-armure : la poise subie est réduite de moitié.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Dead", "Acteur mort.");

	// --- Attaques du joueur ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Player_Light, "Attack.Player.Light", "Attaque légère du joueur.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Player_Heavy, "Attack.Player.Heavy", "Attaque lourde du joueur.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Player_Charged, "Attack.Player.Charged", "Attaque chargée du joueur.");

	// --- Attaques du dragon ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_Bite, "Attack.Dragon.Bite", "Morsure (parable).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_ClawSwipe, "Attack.Dragon.ClawSwipe", "Coup de griffe en deux temps.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_TailSweep, "Attack.Dragon.TailSweep", "Fouet caudal.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_WingBuffet, "Attack.Dragon.WingBuffet", "Rafale d'ailes (repousse).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_BreathCone, "Attack.Dragon.BreathCone", "Souffle frontal en cône.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_BreathSweep, "Attack.Dragon.BreathSweep", "Souffle balayant.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_Dive, "Attack.Dragon.Dive", "Piqué.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_AerialStrafe, "Attack.Dragon.AerialStrafe", "Souffle en vol rasant.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_EmberRain, "Attack.Dragon.EmberRain", "Pluie de braises.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_Grab, "Attack.Dragon.Grab", "Saisie (imblocable).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Attack_Dragon_Cataclysm, "Attack.Dragon.Cataclysm", "Embrasement total (imblocable).");

	// --- Phases de boss ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Boss_Phase_1, "Boss.Phase.1", "Boss en phase 1.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Boss_Phase_2, "Boss.Phase.2", "Boss en phase 2.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Boss_Phase_3, "Boss.Phase.3", "Boss en phase 3 (enragé).");

	// --- Construction : matériaux ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Material_Wood, "Building.Material.Wood", "Matériau bois (T1).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Material_Stone, "Building.Material.Stone", "Matériau pierre (T2).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Material_Iron, "Building.Material.Iron", "Matériau fer forgé (T3).");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Material_Draconic, "Building.Material.Draconic", "Matériau draconique (T4).");

	// --- Construction : catégories ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Foundation, "Building.Category.Foundation", "Fondation.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Floor, "Building.Category.Floor", "Sol / plafond.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Wall, "Building.Category.Wall", "Mur.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Roof, "Building.Category.Roof", "Toit.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Pillar, "Building.Category.Pillar", "Pilier.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Stairs, "Building.Category.Stairs", "Escalier.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Furniture, "Building.Category.Furniture", "Mobilier.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_Category_Workstation, "Building.Category.Workstation", "Établi / poste de travail.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Building_NoBuildZone, "Building.NoBuildZone", "Zone où la construction est interdite.");

	// --- Événements de progression ---
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_Killed_Guardian, "Event.Boss.Killed.Guardian", "Le Gardien a été vaincu.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_Killed_Seraph, "Event.Boss.Killed.Seraph", "Le Séraphin a été vaincu.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Boss_Killed_Dragon, "Event.Boss.Killed.Dragon", "Vaelthyrax a été vaincu.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Location_Discovered_SunkenLibrary, "Event.Location.Discovered.SunkenLibrary", "Bibliothèque engloutie découverte.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Location_Discovered_AshAltar, "Event.Location.Discovered.AshAltar", "Autel de cendre découvert.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Location_Discovered_StarWell, "Event.Location.Discovered.StarWell", "Puits des étoiles découvert.");
}
