// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "EMBDragonTypes.h"
#include "EMBCreatureTypes.generated.h"

class UDataTable;

/**
 * Les créatures réutilisent le format de ligne d'attaque des boss (FEMBDragonAttackRow, alias FEMBBossAttackRow) :
 * les CSV DT_GhoulAttacks / DT_RockWyvernAttacks / DT_SylvadracAttacks ont exactement les colonnes de
 * DT_ColossusAttacks.csv. Colonnes utilisées par une créature légère (AEMBCreatureBase) :
 *  - Kind = Melee : cône frontal (MaxRange, HalfAngleDeg) ; Kind = Charge : bond (lunge) lancé à la fin du télégraphe,
 *    atteignant la cible à HitTimes[0], impact en sphère de rayon EffectRadius ;
 *  - MinRange / MaxRange / HalfAngleDeg, TelegraphTime, HitTimes (secondes depuis le début, télégraphe inclus),
 *    Damage, PoiseDamage, DamageType, bParryable, bUnblockable, BaseWeight, Cooldown, Montage.
 * AllowedPhases doit contenir 1 (les créatures n'ont qu'une phase).
 */
using FEMBCreatureAttackRow = FEMBDragonAttackRow;

/**
 * Gameplay Tags natifs du bestiaire (référentiel §16). Même convention que EMBTags / EMBBossTags : résoudre avec
 * EMBCreatureTags::Attack_Ghoul_Claw.GetTag() à l'usage (jamais dans un constructeur de CDO).
 */
namespace EMBCreatureTags
{
	// --- Goule de cendre ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Ghoul_Claw);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Ghoul_Lunge);

	// --- Wyverneau des rocs ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Wyvern_Bite);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Wyvern_LowSwoop);

	// --- Sylvadrac ---
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Sylvadrac_Bite);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Sylvadrac_LowDive);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Sylvadrac_PoisonCloud);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Sylvadrac_Roar);
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Attack_Sylvadrac_Poison);

	/**
	 * Déclencheur joueur accordé aux participants à la mort du Sylvadrac. Ce N'EST PAS l'événement monde
	 * (GetDefeatedEventTag du Sylvadrac est volontairement invalide : le dragon réapparaît, voir EMBTerritorialDragon.h).
	 */
	EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Creature_Killed_Sylvadrac);
}

/** Ligne de la DataTable DT_Creatures (import depuis Data/DT_Creatures.csv). Distances en cm, vitesses en cm/s. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBCreatureRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identité")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1.0"))
	float MaxHealth = 100.f;

	/** Poise (résistance à l'étourdissement) de la créature. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats", meta = (ClampMin = "1.0"))
	float MaxPoise = 20.f;

	/** Vitesse de patrouille. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Déplacement")
	float WalkSpeed = 150.f;

	/** Vitesse de poursuite, de fuite et de retour au point d'apparition. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Déplacement")
	float RunSpeed = 450.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA")
	float PatrolRadius = 1500.f;

	/** Portée de vue (avant multiplicateur de visibilité météo). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA")
	float SightRadius = 2500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA")
	float HearingRadius = 1200.f;

	/** Distance maximale au point d'apparition : au-delà, la poursuite est abandonnée et la créature rentre. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA")
	float LeashRadius = 4000.f;

	/** Fuit sous cette fraction de PV (0 = ne fuit jamais). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FleeHealthFraction = 0.f;

	/** Fuit une source de chaleur (feu de camp, forge) : voir EMBCreatureDefaults::FleeHeatBonusThreshold. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "IA")
	bool bFleesFire = false;

	/** Vol bas : mode de déplacement MOVE_Flying et mesh surélevé de HoverHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Déplacement")
	bool bFlying = false;

	/** XP accordée à chaque joueur ayant infligé des dégâts dans les 30 dernières secondes (à 10 000 cm au plus). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Récompense")
	int32 XPReward = 0;

	/** Ligne de DT_ResourceNodes de la carcasse (GhoulRemains, RockWyvernCarcass, SylvadracCarcass). Vide : aucune. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Récompense")
	FName CarcassNodeRow;

	/** DataTable des attaques (FEMBCreatureAttackRow). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	TSoftObjectPtr<UDataTable> AttackTable;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = "1"))
	int32 GroupSizeMin = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = "1"))
	int32 GroupSizeMax = 1;

	/** Corrompu : poids d'apparition doublé la nuit (EMBCreatureDefaults::NightSpawnWeightMultiplier). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn")
	bool bNightBoost = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identité")
	FText Description;
};

namespace EMBCreatureData
{
	/**
	 * Lit la ligne RowName de la table (chargement synchrone d'une table minuscule). Vérifie le type de ligne
	 * (FindRow sur une table d'une autre structure lirait de la mémoire invalide). Retourne faux si absente.
	 */
	EMBERFALL_API bool FindRow(const TSoftObjectPtr<UDataTable>& Table, FName RowName, FEMBCreatureRow& OutRow);

	/** Chemin par défaut de DT_Creatures (même convention que les autres tables : /Game/Data/DT_X.DT_X). */
	EMBERFALL_API FSoftObjectPath GetDefaultTablePath();
}

/**
 * Constantes du bestiaire (référentiel §16). Les valeurs marquées (*) ne figurent pas dans le référentiel : ce sont
 * des choix d'implémentation, à équilibrer en playtest.
 */
namespace EMBCreatureDefaults
{
	// --- IA commune (§16) ---
	// Patrouille (1 500), laisse (4 000) : valeurs de DT_Creatures, pas de constante de code.
	inline constexpr float SightRadius = 2500.f;               // repli du constructeur du contrôleur
	inline constexpr float HearingRadius = 1200.f;             // idem
	inline constexpr float FleeHeatBonusThreshold = 10.f;      // °C : source de chaleur >= 10 °C (feu de camp +15, forge +10)

	// --- XP (§16) ---
	inline constexpr float ContributorMemory = 30.f;           // s : dégâts pris en compte pour l'XP
	inline constexpr float XPParticipantRadius = 10000.f;      // cm

	// --- Carcasse (§12 / §13) ---
	inline constexpr float CarcassLifeSpan = 600.f;            // s : SpawnDynamicNode(..., 600)

	// --- Spawn (§16) ---
	inline constexpr int32 ServerBudget = 40;                  // créatures vivantes simultanées côté serveur
	inline constexpr float SpawnMaxPlayerDistance = 8000.f;    // un joueur doit être à <= 8 000 cm du spawner
	inline constexpr float SpawnMinPlayerDistance = 2500.f;    // aucun joueur à < 2 500 cm du point d'apparition
	inline constexpr float NightSpawnWeightMultiplier = 2.f;

	// --- Significance (§16) ---
	inline constexpr float FullDetailDistance = 3000.f;        // < 3 000 : IA et animation pleines
	inline constexpr float ReducedDetailDistance = 8000.f;     // 3 000-8 000 : tick IA 0,25 s, URO ; > 8 000 : IA en pause
	inline constexpr float ReleaseDistance = 15000.f;          // > 15 000 : créature rendue au spawner
	inline constexpr float ReducedAITickInterval = 0.25f;

	// --- Choix d'implémentation (*) ---
	inline constexpr float PoiseRegenRate = 10.f;              // (*) /s
	inline constexpr float PoiseRegenDelay = 2.f;              // (*) s
	inline constexpr float HoverHeight = 120.f;                // (*) surélévation du mesh d'une créature volante
	inline constexpr float DeathLifeSpan = 8.f;                // (*) s avant disparition du cadavre (la carcasse reste 600 s)
	inline constexpr float AttackRecoveryTail = 0.4f;          // (*) s de récupération après le dernier impact
	inline constexpr float FacingLockLeadTime = 0.25f;         // (*) orientation figée avant le premier impact (esquivable)
	inline constexpr float LungeLift = 120.f;                  // (*) cm/s de composante verticale d'un bond (quitte le sol)
	inline constexpr float LoseSightFactor = 1.15f;            // (*) rayon de perte de vue = vue x 1,15
	inline constexpr float HearingMinSpeed = 150.f;            // (*) cm/s : un joueur plus lent que ça n'est pas entendu
	inline constexpr float AggroMemory = 6.f;                  // (*) s : le dernier agresseur reste une cible
	inline constexpr float ReturnHomeReachedRadius = 250.f;    // (*) cm
	inline constexpr float FleeDistance = 1800.f;              // (*) cm parcourus à chaque fuite
	inline constexpr float FleeMaxDuration = 6.f;              // (*) s
	inline constexpr float SignificanceMaxDistance = 100000.f; // (*) significance = SignificanceMaxDistance - distance
	inline constexpr float SignificanceUpdateInterval = 0.5f;  // (*) s
	inline constexpr float SignificanceHysteresis = 250.f;     // (*) cm de marge anti-oscillation entre deux paliers
}

/**
 * Constantes du Sylvadrac (référentiel §16). (*) = choix d'implémentation.
 * Les PV, la posture, l'XP et la carcasse sont relus dans DT_Creatures (ligne "Sylvadrac") : ces valeurs ne servent
 * que de repli si la table est introuvable.
 */
namespace EMBSylvadracDefaults
{
	inline constexpr float MaxHealthSolo = 25000.f;
	inline constexpr float MaxPoise = 800.f;
	inline constexpr float PoiseRegenRate = 30.f;              // (*) /s
	inline constexpr float PoiseRegenDelay = 3.f;              // (*) s
	inline constexpr float WalkSpeed = 500.f;                  // (*)
	inline constexpr float RunSpeed = 1100.f;                  // (*)
	inline constexpr int64 XPReward = 20000;

	// Territoire
	inline constexpr float TerritoryRadius = 30000.f;          // 300 m autour du nid
	inline constexpr float AggressionDelay = 20.f;             // s de présence avant l'attaque
	inline constexpr float RespawnDelay = 144.f * 60.f;        // 3 journées de jeu de 48 min = 144 min
	inline constexpr float RoarCooldown = 30.f;                // (*) s entre deux rugissements d'avertissement
	inline constexpr float ReturnDelay = 8.f;                  // (*) s sans joueur dans le territoire avant le retour au nid
	inline constexpr float LeashMargin = 3000.f;               // (*) cm au-delà du territoire avant le retour forcé
	inline constexpr float NestArrivalRadius = 800.f;          // (*) cm
	inline constexpr float TerritoryTickInterval = 0.5f;       // (*) s
	inline constexpr float IdleNetUpdateFrequency = 10.f;      // (*) Hz hors combat (la base fixe 30)
	inline constexpr float CombatNetUpdateFrequency = 30.f;    // (*) Hz en combat
	inline constexpr float RegenFractionPerSecond = 0.02f;     // (*) 2 % des PV max par seconde hors combat
	inline constexpr float RoarDuration = 3.f;                 // (*) s (télégraphe visuel du rugissement)
	inline constexpr float FallbackSpawnerMinPlayerDistance = 5000.f; // (*)

	// Poison (§16 : 20 dégâts/s, nuage persistant 8 s)
	inline constexpr float PoisonDps = 20.f;
	inline constexpr float PoisonTickInterval = 0.25f;         // (*) 5 dégâts par tick
	inline constexpr float PoisonDuration = 6.f;               // (*) s après un coup de morsure, rafraîchie sans cumul
	inline constexpr float CloudDuration = 8.f;
	inline constexpr float CloudRadius = 450.f;                // (*) si la ligne d'attaque n'a pas d'EffectRadius
	inline constexpr float CloudPoisonRefresh = 1.f;           // (*) s de poison ajoutées à chaque tick dans le nuage
	inline constexpr float CloudPredictionTime = 0.6f;         // (*) anticipation de la course du joueur visé
	inline constexpr float CloudPredictionMax = 600.f;         // (*) cm

	// Piqué bas
	inline constexpr float LowDiveFlightTime = 0.7f;           // (*) s entre le bond et l'impact
	inline constexpr float LowDiveLift = 180.f;                // (*) cm/s verticaux du bond
	inline constexpr float LowDiveDefaultDistance = 1500.f;    // (*) sans cible
	inline constexpr float LowDiveImpactFlash = 0.4f;          // (*) s
}
