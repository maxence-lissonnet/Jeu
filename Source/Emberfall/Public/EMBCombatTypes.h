// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.generated.h"

class AActor;
class UAnimMontage;

// ------------------------------------------------------------------------------------------------
// Énumérations (contrat 3.2)
// ------------------------------------------------------------------------------------------------

UENUM(BlueprintType)
enum class EEMBCombatState : uint8
{
	Idle,
	LightAttack,
	HeavyAttack,
	ChargedAttack,
	Dodge,
	Parry,
	Block,
	Staggered,
	Recovery,
	Dead
};

UENUM(BlueprintType)
enum class EEMBCombatInput : uint8
{
	None,
	LightAttack,
	HeavyAttack,
	ChargedAttackStart,
	ChargedAttackRelease,
	Dodge,
	Parry,
	BlockStart,
	BlockEnd
};

UENUM(BlueprintType)
enum class EEMBHitResult : uint8
{
	Hit,
	Blocked,
	Parried,
	Dodged,
	GuardBroken,
	Killed,
	Ignored
};

UENUM(BlueprintType)
enum class EEMBDamageType : uint8
{
	Physical,
	Fire,
	Frost,
	Lightning,
	Corruption
};

// ------------------------------------------------------------------------------------------------
// Structures
// ------------------------------------------------------------------------------------------------

/** Données d'un coup porté. Construite par l'attaquant, résolue côté serveur par la cible. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBHitData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	TObjectPtr<AActor> Instigator = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	float Damage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	float PoiseDamage = 0.f;

	/** Coût d'endurance si l'attaque est bloquée (0 => Damage * 0,5). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	float StaminaDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	EEMBDamageType DamageType = EEMBDamageType::Physical;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	FVector HitLocation = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	FVector HitDirection = FVector::ForwardVector;

	/** Points faibles des boss. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	FName HitBoneName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	bool bUnblockable = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	bool bUnparryable = false;

	/**
	 * Dégâts continus (lave, brûlure, zones de feu) : dégâts directs sans esquive, garde, parade,
	 * counter-hit ni retour de coup multicast (v1.1 du référentiel).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	bool bDamageOverTime = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hit")
	FGameplayTag AttackTag;
};

/** Paramètres d'une fenêtre de coup (portés par EMBAnimNotifyState_HitWindow). */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBHitWindowParams
{
	GENERATED_BODY()

	/** Rayon (cm) de la sphère balayée le long de la lame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HitWindow", meta = (ClampMin = "1"))
	float SphereRadius = 35.f;

	/** Multiplicateur supplémentaire appliqué aux dégâts de l'attaque en cours. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HitWindow", meta = (ClampMin = "0"))
	float DamageScale = 1.f;

	/** Multiplicateur supplémentaire appliqué aux dégâts de poise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "HitWindow", meta = (ClampMin = "0"))
	float PoiseScale = 1.f;
};

/** Définition d'une attaque du joueur. Les valeurs par défaut viennent de EMBCombatDefaults. */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBAttackDefinition
{
	GENERATED_BODY()

	/** Coût d'endurance, consommé au déclenchement (au relâchement pour l'attaque chargée). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0"))
	float StaminaCost = 0.f;

	/** Multiplicateur appliqué aux dégâts de base de l'arme (WeaponBaseDamage). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0"))
	float DamageMultiplier = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0"))
	float PoiseDamage = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack", meta = (ClampMin = "0.1"))
	float PlayRate = 1.f;

	/** Montage joué ; doit contenir les notifies HitWindow (et ComboWindow pour les attaques légères). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
	TObjectPtr<UAnimMontage> Montage = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Attack")
	FGameplayTag AttackTag;
};

// ------------------------------------------------------------------------------------------------
// Constantes de combat (référentiel 00_Constantes_et_Contrats.md, sections 4.1 à 4.3)
// ------------------------------------------------------------------------------------------------

/**
 * Toutes les valeurs de gameplay du combat. Les durées sont en secondes ; le commentaire donne la
 * valeur en frames à 60 fps. Valeurs de conception à équilibrer en playtest.
 * Pour modifier une valeur : changer le référentiel d'abord, puis ce fichier.
 */
namespace EMBCombatDefaults
{
	/** Durée d'une frame à 60 fps (référence de toutes les fenêtres). */
	inline constexpr float FrameTime = 1.f / 60.f;

	// ---- 4.1 Attributs de base (niveau 1) ----
	inline constexpr float MaxHealth = 500.f;
	inline constexpr float MaxStamina = 100.f;
	inline constexpr float MaxPoise = 50.f;
	inline constexpr float StaminaRegenRate = 30.f;              // par seconde
	inline constexpr float StaminaRegenDelay = 0.8f;             // 48 f
	inline constexpr float ExhaustedRegenDelay = 1.5f;           // 90 f
	inline constexpr float BlockStaminaRegenMultiplier = 0.35f;
	inline constexpr float PoiseRegenRate = 15.f;                // par seconde
	inline constexpr float PoiseRegenDelay = 2.0f;               // 120 f
	inline constexpr float InputBufferTime = 0.2f;               // 12 f
	inline constexpr float SprintStaminaPerSecond = 8.f;
	inline constexpr float JumpStaminaCost = 10.f;

	// ---- 4.2 Arme de référence ----
	inline constexpr float WeaponBaseDamage = 80.f;              // épée longue

	// Attaque légère (combo 3 coups)
	inline constexpr int32 LightComboLength = 3;
	inline constexpr float LightStaminaCost = 14.f;
	inline constexpr float LightDamageMultiplier = 1.0f;
	inline constexpr float LightThirdHitDamageMultiplier = 1.2f;
	inline constexpr float LightPoiseDamage = 12.f;
	inline constexpr float LightTotalDuration = 36 * FrameTime;  // 0,60 s (36 f)
	inline constexpr float LightActiveStart = 12 * FrameTime;    // 0,20 s (frame 12)
	inline constexpr float LightActiveEnd = 18 * FrameTime;      // 0,30 s (actif 12-17, recovery dès la frame 18)
	inline constexpr float LightComboWindowOpen = 18 * FrameTime;   // 0,30 s (frame 18)
	inline constexpr float LightComboWindowClose = 31 * FrameTime;  // 0,517 s (fenêtre 18-30 incluse)

	// Attaque lourde
	inline constexpr float HeavyStaminaCost = 26.f;
	inline constexpr float HeavyDamageMultiplier = 1.45f;
	inline constexpr float HeavyPoiseDamage = 25.f;
	inline constexpr float HeavyTotalDuration = 60 * FrameTime;  // 1,00 s (60 f)
	inline constexpr float HeavyActiveStart = 24 * FrameTime;    // 0,40 s (frame 24)
	inline constexpr float HeavyActiveEnd = 32 * FrameTime;      // 0,533 s (actif 24-31)
	inline constexpr float HeavyHyperArmorStart = 6 * FrameTime; // 0,10 s (frame 6)
	inline constexpr float HeavyHyperArmorEnd = 32 * FrameTime;  // 0,533 s (hyper-armure 6-31)

	// Attaque chargée
	inline constexpr float ChargedStaminaCost = 38.f;            // au relâchement
	inline constexpr float ChargedDamageMultiplierMin = 1.6f;
	inline constexpr float ChargedDamageMultiplierMax = 2.4f;
	inline constexpr float ChargedPoiseDamageMin = 35.f;
	inline constexpr float ChargedPoiseDamageMax = 55.f;
	inline constexpr float ChargedMinChargeTime = 0.4f;          // 24 f : en dessous, attaque lourde
	inline constexpr float ChargedMaxChargeTime = 1.2f;          // 72 f : plafond du multiplicateur
	inline constexpr float ChargedStrikeDuration = 54 * FrameTime; // 0,90 s (54 f)
	// Hypothèse (non précisée au référentiel) : fenêtre active de la frappe chargée, utilisée
	// uniquement en mode dégradé (montage absent). Les montages réels pilotent via HitWindow.
	inline constexpr float ChargedStrikeActiveStart = 20 * FrameTime; // 0,333 s (frame 20)
	inline constexpr float ChargedStrikeActiveEnd = 30 * FrameTime;   // 0,50 s (frame 30)

	// Roulade
	inline constexpr float DodgeStaminaCost = 18.f;
	inline constexpr float DodgeHeavyLoadStaminaCost = 22.f;
	inline constexpr float DodgeTotalDuration = 42 * FrameTime;  // 0,70 s (42 f)
	inline constexpr float DodgeIFrameStart = 3 * FrameTime;     // 0,05 s (frame 3)
	inline constexpr float DodgeIFrameEndLight = 16 * FrameTime;   // i-frames 3-15 : 13 f = 0,217 s
	inline constexpr float DodgeIFrameEndMedium = 14 * FrameTime;  // i-frames 3-13 : 11 f
	inline constexpr float DodgeIFrameEndHeavy = 12 * FrameTime;   // i-frames 3-11 : 9 f
	inline constexpr float DodgeCancelTime = 30 * FrameTime;     // 0,50 s (frame 30)
	inline constexpr float LoadLightMax = 0.30f;                 // charge < 30 % : roulade légère
	inline constexpr float LoadMediumMax = 0.70f;                // 30-70 % : moyenne
	inline constexpr float LoadHeavyMax = 1.00f;                 // 70-100 % : lourde ; > 100 % : pas de roulade

	// Parade
	inline constexpr float ParryStaminaCost = 10.f;
	inline constexpr float ParryTotalDuration = 45 * FrameTime;  // 0,75 s (45 f)
	inline constexpr float ParryPerfectStart = 4 * FrameTime;    // 0,067 s (frame 4)
	inline constexpr float ParryPerfectEnd = 14 * FrameTime;     // 0,233 s (fenêtre 4-13 incluse = 10 f = 0,167 s)
	inline constexpr float ParryWhiffedVulnerableEnd = 45 * FrameTime; // parade ratée : vulnérable 14-44

	// Garde
	// Réduction de la garde (bouclier) selon le type de dégâts (référentiel v1.1)
	inline constexpr float BlockReductionPhysical = 0.85f;       // 85 %
	inline constexpr float BlockReductionElemental = 0.50f;      // feu, givre, foudre : 50 %
	inline constexpr float BlockReductionCorruption = 0.30f;     // corruption : 30 %
	inline constexpr float BlockRaiseTime = 6 * FrameTime;       // 0,10 s (6 f)
	inline constexpr float GuardBreakStaggerDuration = 72 * FrameTime; // 1,2 s (72 f)

	// Poise brisée
	inline constexpr float PoiseBreakStaggerDuration = 48 * FrameTime; // 0,8 s (48 f)

	// ---- 4.3 Punitions et modificateurs ----
	inline constexpr float CounterHitDamageMultiplier = 1.25f;
	inline constexpr float HyperArmorPoiseMultiplier = 0.5f;
	inline constexpr float ParryPostureDamage = 35.f;
	inline constexpr float ParryRiposteWindow = 1.5f;            // 90 f : riposte sur ennemi standard paré
	inline constexpr float BossPostureBreakDuration = 4.0f;      // 240 f
	inline constexpr float CriticalRiposteMultiplier = 3.0f;     // riposte sur boss étourdi
}
