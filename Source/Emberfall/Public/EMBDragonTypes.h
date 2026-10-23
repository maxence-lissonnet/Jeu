// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBDragonTypes.generated.h"

class UAnimMontage;
class UNiagaraSystem;

/** Famille d'exécution d'une attaque : détermine quelle branche du Behavior Tree la joue. */
UENUM(BlueprintType)
enum class EEMBDragonAttackKind : uint8
{
	Melee     UMETA(DisplayName = "Mêlée (morsure, griffe, queue)"),
	Breath    UMETA(DisplayName = "Souffle"),
	Dive      UMETA(DisplayName = "Piqué"),
	Area      UMETA(DisplayName = "Zone (rafale, pluie de braises)"),
	Grab      UMETA(DisplayName = "Saisie"),
	Scripted  UMETA(DisplayName = "Scripté (Cataclysm)")
};

/** Variante de souffle (utilisée par BTTask_EMBDragonBreath, ignorée pour les autres familles). */
UENUM(BlueprintType)
enum class EEMBBreathMode : uint8
{
	Cone      UMETA(DisplayName = "Cône frontal"),
	Sweep     UMETA(DisplayName = "Balayage"),
	Aerial    UMETA(DisplayName = "Aérien (ligne de feu)"),
	Cataclysm UMETA(DisplayName = "Cataclysm (embrasement total)")
};

/**
 * Ligne de la DataTable DT_DragonAttacks (import depuis Data/DT_DragonAttacks.csv).
 * Toutes les distances sont en cm, toutes les durées en secondes (avant multiplicateurs de phase).
 * Le nom de ligne est libre ; la clé fonctionnelle est AttackTag.
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBDragonAttackRow : public FTableRowBase
{
	GENERATED_BODY()

	/** Identifiant de l'attaque (Attack.Dragon.*). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identité")
	FGameplayTag AttackTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Identité")
	EEMBDragonAttackKind Kind = EEMBDragonAttackKind::Melee;

	/** Phases (1, 2, 3) dans lesquelles l'attaque peut être tirée. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	TArray<int32> AllowedPhases;

	/** Portée minimale / maximale mesurée centre du dragon -> cible, à plat (cm). MaxRange <= 0 : portée illimitée. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	float MinRange = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	float MaxRange = 0.f;

	/** Demi-angle frontal accepté (degrés, 0..180). 180 = 360 degrés. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float HalfAngleDeg = 180.f;

	/** Attaque arrière : valide uniquement si |angle| >= RearMinAngleDeg (fouet caudal). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	bool bRearOnly = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float RearMinAngleDeg = 110.f;

	/** Durée du télégraphe (s) : sert de base aux tâches latentes ; multipliée par 0,9 en phase 3. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	float TelegraphTime = 1.f;

	/** Fenêtre d'esquive (s depuis le début de l'attaque). Informatif (UI / équilibrage / IA alliée). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	float DodgeWindowStart = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	float DodgeWindowEnd = 0.f;

	/** Seconde fenêtre d'esquive (griffe en deux coups). 0/0 = absente. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	float DodgeWindow2Start = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	float DodgeWindow2End = 0.f;

	/**
	 * Instants d'impact (s depuis le début, montage à vitesse 1) pour les attaques de mêlée / saisie / rafale.
	 * Vide pour les souffles, le piqué et la pluie de braises (gérés par leur propre logique).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing")
	TArray<float> HitTimes;

	/** Dégâts par coup (par tick pour les souffles). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float Damage = 0.f;

	/** Dégâts de poise par coup (par tick pour les souffles). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float PoiseDamage = 0.f;

	/** Dégâts d'endurance par coup / par tick si l'attaque est bloquée (souffles : 10 par tick). 0 : Damage x 0,5. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float StaminaDamage = 0.f;

	/** Rayon d'effet (piqué : 600, pluie de braises : 300, zones de feu aériennes). 0 = non utilisé. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float EffectRadius = 0.f;

	/** Repoussement (rafale d'ailes : 1 200 cm). 0 = aucun. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float KnockbackDistance = 0.f;

	/** Durée de la phase active (souffle : 2,5 s cône / 2,0 s balayage ; pluie de braises : 6 s). 0 = valeur par défaut du code. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	float ActiveDuration = 0.f;

	/** Nombre d'impacts répartis sur ActiveDuration (pluie de braises : 12). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	int32 ImpactCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	EEMBDamageType DamageType = EEMBDamageType::Physical;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	bool bParryable = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	bool bUnblockable = false;

	/** Applique la brûlure (15/s pendant 4 s) aux cibles touchées. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Dégâts")
	bool bAppliesBurn = false;

	/** Exclue du tirage pondéré : déclenchée par le script du boss (Cataclysm). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	bool bScripted = false;

	/** Poids de base du tirage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	float BaseWeight = 10.f;

	/** Multiplicateurs de poids par phase [P1, P2, P3]. Tableau vide ou trop court : 1,0. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	TArray<float> PhaseWeights;

	/** Recharge (s) démarrant à la sélection ; multipliée par 0,8 en phase 3. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sélection")
	float Cooldown = 5.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Assets")
	TSoftObjectPtr<UAnimMontage> Montage;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Assets")
	EEMBBreathMode BreathMode = EEMBBreathMode::Cone;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Assets")
	TSoftObjectPtr<UNiagaraSystem> VFX;

	bool IsAllowedInPhase(const int32 Phase) const
	{
		return AllowedPhases.Contains(Phase);
	}

	float GetPhaseWeight(const int32 Phase) const
	{
		return PhaseWeights.IsValidIndex(Phase - 1) ? PhaseWeights[Phase - 1] : 1.f;
	}
};

/**
 * Constantes du dragon final Vaelthyrax (docs/00_Constantes_et_Contrats.md, section 6).
 * Valeurs de conception à équilibrer en playtest. Les constantes marquées (*) ne figurent pas dans
 * la section 6 : ce sont des choix d'implémentation, signalés pour relecture.
 */
namespace EMBDragonDefaults
{
	// --- Santé / posture ---
	constexpr float MaxHealthSolo = 90000.f;
	constexpr float CoopHealthBonusPerExtraPlayer = 0.6f; // PV x (1 + 0,6 x (joueurs - 1))
	constexpr int32 MaxCoopPlayers = 4;
	constexpr float MaxPoise = 1500.f;
	constexpr float PoiseRegenRate = 40.f;   // par seconde
	constexpr float PoiseRegenDelay = 4.f;   // secondes sans dégâts de posture
	// Alias vers la source unique (EMBCombatTypes.h) : pas de copie de valeur.
	constexpr float PostureBreakStunDuration = EMBCombatDefaults::BossPostureBreakDuration;
	constexpr float CriticalHitMultiplier = EMBCombatDefaults::CriticalRiposteMultiplier;

	// --- Phases ---
	constexpr float Phase2HealthThreshold = 0.70f;
	constexpr float Phase3HealthThreshold = 0.35f;
	constexpr float PhaseTransitionDuration = 5.f;
	constexpr float EnragedPlayRate = 1.15f;
	constexpr float EnragedCooldownMultiplier = 0.8f;
	constexpr float EnragedTelegraphMultiplier = 0.9f;

	// Progression : XP du Dragon et rayon des participants viennent de UEMBProgressionSettings (source unique, §7.2).

	// --- Déplacements ---
	constexpr float WalkSpeed = 600.f;
	constexpr float FlySpeed = 2400.f;
	constexpr float DiveSpeed = 4500.f;

	// --- Points faibles ---
	constexpr float WeakPointHead = 1.3f;
	constexpr float WeakPointBelly = 1.15f;
	constexpr float WeakPointWings = 1.0f;
	constexpr float WeakPointTail = 0.8f;

	// --- Sélection d'attaque ---
	constexpr float RepeatPenaltyLast = 0.25f;
	constexpr float RepeatPenaltyPrevious = 0.6f;
	constexpr float WingBuffetCrowdMultiplier = 3.f;
	constexpr float WingBuffetCrowdRadius = 800.f;
	constexpr int32 WingBuffetCrowdCount = 2;
	constexpr float DistanceFactorEdgeFloor = 0.25f;     // (*) facteur aux bords de la plage (1 au centre, 0 hors plage)
	constexpr float AngleFactorEdgeFloor = 0.5f;         // (*) idem pour l'angle frontal

	// --- Souffles ---
	constexpr float BreathTicksPerSecond = 8.f;
	constexpr float BeamFullAngleDeg = 20.f;             // cône de 20 degrés
	constexpr float ConeBreathDuration = 2.5f;
	constexpr float SweepArcDeg = 120.f;
	constexpr float SweepBreathDuration = 2.0f;
	constexpr float AerialStrafeDuration = 2.5f;         // (*) non précisée dans la section 6
	constexpr float AerialBeamPitchDeg = 35.f;           // (*) inclinaison du faisceau vers le sol
	constexpr float AerialStrafeAltitude = 600.f;        // (*) altitude du vol rasant
	constexpr float AerialZoneDuration = 8.f;
	constexpr float AerialZoneDps = 20.f;
	constexpr float AerialZoneRadius = 250.f;            // (*)
	constexpr float CataclysmChargeTime = 4.f;
	constexpr float CataclysmRadius = 15000.f;           // (*) couvre toute l'arène
	constexpr float CataclysmAfterglow = 1.0f;           // (*) temps de maintien du VFX après l'explosion
	constexpr float CataclysmAltitude = 1800.f;          // (*)
	constexpr float CataclysmFirstDelayAfterPhase3 = 20.f;
	constexpr float CataclysmInterval = 75.f;

	// --- Brûlure ---
	constexpr float BurnDps = 15.f;
	constexpr float BurnDuration = 4.f;
	constexpr float BurnTickInterval = 0.25f;            // (*) 4 ticks/s, 3,75 dégâts par tick

	// --- Piqué ---
	constexpr float DiveTakeoffDuration = 2.0f;
	constexpr float DiveMarkDuration = 1.5f;
	constexpr float DiveFlashDuration = 0.35f;
	constexpr float DiveImpactRadius = 600.f;
	constexpr float DiveVulnerableDuration = 2.5f;
	constexpr float DiveCruiseAltitude = 1800.f;         // (*)
	constexpr float DivePredictionLead = 0.5f;           // (*) anticipation de la position de la cible

	// --- Pluie de braises ---
	constexpr float EmberMarkerLead = 1.2f;
	constexpr float EmberImpactRadius = 300.f;

	// --- Vulnérabilité après le piqué (v1.1) ---
	constexpr float VulnerableDamageMultiplier = 1.5f;
	constexpr float BreathStaminaDamagePerTick = 10.f;   // v1.1 : garde tenable environ 1,2 s face au souffle

	// --- Arène ---
	constexpr float LavaDps = 25.f;

	// --- Foudre de phase 3 (v1.1) ---
	constexpr float LightningInterval = 3.0f;            // un éclair toutes les 3,0 s
	constexpr float LightningMarkerDuration = 1.5f;      // télégraphe au sol
	constexpr float LightningDamage = 150.f;
	constexpr float LightningRadius = 250.f;
	constexpr float LightningMaxOffset = 400.f;          // décalage de 0 à 400 cm autour du joueur visé
	constexpr float LightningImpactFlashDuration = 0.35f; // (*) flash d'impact diffusé aux clients

	// --- IA ---
	constexpr float PerceptionRadius = 8000.f;
	constexpr float ThreatCountRadius = 2500.f;          // (*) rayon de comptage des joueurs "proches"
}
