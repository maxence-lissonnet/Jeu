// Copyright ADWEB Conseil. Tous droits réservés.

/*
 * ===============================================================================================
 * STRUCTURE DU BEHAVIOR TREE ATTENDU : BT_Colossus (Blackboard : BB_Colossus, mêmes clés que le dragon)
 * ===============================================================================================
 * Contrôleur : AEMBBossAIController (ou une classe fille). Priorité décroissante de haut en bas.
 *
 * Root
 * └─ Selector "Racine"                       [Service : BTService_EMBUpdateCombatContext (0,2 s ± 0,05)]
 *    ├─ 1. Sequence "Mort"                   [Deco BB bIsDead == true, aborts Both]          -> Wait long
 *    ├─ 2. Sequence "Transition de phase"    [Deco BB bIsTransitioning == true, aborts Both] -> Wait 0,25 s
 *    ├─ 3. Sequence "Étourdi"                [Deco BB bIsStunned == true, aborts Both]       -> Wait 0,25 s
 *    │      (posture brisée 4 s, ou 3 s après une charge qui percute un obélisque / un mur)
 *    ├─ 4. Sequence "Combat"                 [Deco BB TargetActor is Set, aborts Self]
 *    │     ├─ BTTask_EMBSelectAttack         (échoue si rien de valide, ou si le Colosse tombe)
 *    │     ├─ Selector "Exécution par type"  (un décorateur BB par enfant, sur la clé entière SelectedAttackKind)
 *    │     │    ├─ [SelectedAttackKind == 6 (Charge)]  BTTask_EMBColossusCharge
 *    │     │    └─ [SelectedAttackKind == 0 (Melee), 3 (Area), 7 (Channel) ou 8 (Rings)]
 *    │     │         Wait Blackboard Time (clé SelectedAttackDuration)  (impacts programmés par le boss)
 *    │     │         Valeurs : Melee 0, Breath 1, Dive 2, Area 3, Grab 4, Scripted 5, Charge 6, Channel 7, Rings 8.
 *    │     │         (dupliquer le noeud avec un décorateur par valeur, ou un décorateur composite "Or")
 *    │     └─ Wait 0,4 à 0,8 s (aléatoire)   (récupération : fenêtre de punition pour le joueur)
 *    └─ 5. Sequence "Repositionnement" (repli) : MoveTo(TargetActor, rayon 500) puis Wait 0,3 s
 *
 * Remarques :
 *  - Rugissement du lion -> Poing-marteau : l'enchaînement est géré par AEMBColossusBoss (GetChainedAttackTag), le BT
 *    n'a rien à faire ; la durée écrite dans SelectedAttackDuration couvre les deux attaques.
 *  - Résonance : canalisation de 3 s interrompue par 150 de posture reçue ; exclue de la sélection sans obélisque intact.
 *  - Ondes runiques : les anneaux sont émis par des timers du boss et se propagent seuls (aucune tâche BT dédiée).
 *  - Aucun décorateur de phase n'est nécessaire : la table d'attaques filtre les phases (AllowedPhases).
 * ===============================================================================================
 */

#pragma once

#include "CoreMinimal.h"
#include "EMBBossBase.h"
#include "EMBColossusBoss.generated.h"

class AEMBColossusArena;

/**
 * Constantes du Colosse d'Ossivaire (référentiel §11.2). Les valeurs marquées (*) ne figurent pas dans le
 * référentiel : ce sont des choix d'implémentation, à équilibrer en playtest.
 */
namespace EMBColossusDefaults
{
	// --- Stats ---
	constexpr float MaxHealthSolo = 12000.f;
	constexpr float MaxPoise = 600.f;
	constexpr float PoiseRegenRate = 25.f;
	constexpr float PoiseRegenDelay = 3.f;
	constexpr float StunDuration = EMBCombatDefaults::BossPostureBreakDuration; // alias : source unique dans EMBCombatTypes.h
	constexpr float WalkSpeed = 350.f;

	// --- Phases ---
	constexpr float Phase2HealthThreshold = 0.50f;
	constexpr float PhaseTransitionDuration = 4.f;
	constexpr float EnragedTelegraphMultiplier = 0.9f;    // P2 : runes embrasées

	// --- Résistances (fraction retranchée ; négatif = faiblesse) ---
	constexpr float ResistPhysical = 0.10f;
	constexpr float ResistFire = 0.40f;
	constexpr float ResistLightning = -0.20f;
	constexpr float ResistCorruption = 0.0f;

	// --- Noyau runique ---
	constexpr float CoreMultiplier = 1.5f;
	constexpr float CoreExposureDuration = 3.f;           // après le Séisme

	// --- Charge de bélier ---
	constexpr float ChargeSpeed = 2000.f;                 // (*) cm/s
	constexpr float ChargeOvershoot = 500.f;              // (*) distance parcourue au-delà de MaxRange si rien n'arrête la charge
	constexpr float ChargeContactMargin = 100.f;          // (*) marge ajoutée au rayon de capsule pour toucher un joueur
	constexpr float ChargeImpactStunDuration = 3.f;

	// --- Ondes runiques ---
	constexpr int32 RuneRingCount = 3;
	constexpr float RuneRingInterval = 0.8f;
	constexpr float RuneRingSpeed = 1000.f;               // cm/s
	constexpr float RuneRingTickInterval = 0.05f;         // (*) 20 Hz : un pas de 50 cm, inférieur à l'épaisseur (150)
	constexpr float RuneRingRecoveryAfterLast = 1.0f;     // (*) le boss récupère 1 s après le dernier anneau émis

	// --- Résonance ---
	constexpr float ResonanceHealFractionPerObelisk = 0.03f;
	constexpr float ResonancePoiseInterrupt = 150.f;

	// --- Obélisques ---
	constexpr float ObeliskMaxHealth = 500.f;
}

/**
 * Le Colosse d'Ossivaire : Gardien terrestre, boss 1 (GDD §9.1, référentiel §11.2). Golem-chimère à deux phases.
 *
 * Fait ce que la base ne fait pas :
 *  - Séisme : expose le noyau runique (point faible conditionnel x1,5) pendant 3 s ;
 *  - Rugissement du lion : enchaîne automatiquement le Poing-marteau ;
 *  - Ondes runiques : 3 anneaux expansifs (timers, jamais de Tick) ; chaque joueur ne peut être touché qu'une fois
 *    par anneau, quand sa distance au centre traverse le rayon courant à ± la demi-épaisseur ;
 *  - Résonance : canalisation de 3 s, soin de 3 % des PV max par obélisque intact, interrompue par 150 de posture
 *    reçue pendant la canalisation ;
 *  - Charge de bélier : exécutée par UBTTask_EMBColossusCharge (API publique ci-dessous).
 * Les obélisques et leur allumage en phase 2 sont gérés par AEMBColossusArena.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBColossusBoss : public AEMBBossBase
{
	GENERATED_BODY()

public:
	AEMBColossusBoss();

	// --- AEMBBossBase ---
	virtual FGameplayTag GetDefeatedEventTag() const override;

	/** La Résonance est exclue de la sélection si aucun obélisque n'est intact (poids 0). */
	virtual bool IsAttackAllowed(const FEMBBossAttackRow& Row) const override;

	// --- Arène et obélisques ---

	/** Appelé par l'arène à sa liaison : le boss lui demande le nombre d'obélisques intacts et ses obstacles de charge. */
	void SetArena(AEMBColossusArena* InArena);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Colossus")
	int32 GetIntactObeliskCount() const;

	/** Un acteur arrêté par la charge est-il un obstacle valide (obélisque, mur de l'arène) ? Sans arène : vrai. */
	bool IsValidChargeObstacle(const AActor* HitActor) const;

	/** Expose le noyau runique pendant Duration secondes (Séisme : 3 s). Serveur. */
	void ExposeCore(float Duration);

	// --- API de la charge (utilisée par UBTTask_EMBColossusCharge) ---

	/** Résout l'impact d'une charge : dégâts à l'obstacle s'il peut en subir, effets visuels, puis étourdi 3 s. Serveur. */
	void ResolveChargeImpact(const FHitResult& BlockingHit, const FEMBBossAttackRow& Row);

protected:
	virtual bool ScheduleCustomEvents(const FEMBBossAttackRow& Row, float TimeScale, float& InOutLastEventTime) override;
	virtual void OnTimedHitExecuted(const FEMBBossAttackRow& Row, int32 HitIndex, const TArray<AActor*>& Landed) override;
	virtual FGameplayTag GetChainedAttackTag(const FEMBBossAttackRow& Row) const override;
	virtual void OnPoiseDamageTaken(float Amount, AActor* InInstigator) override;
	virtual void OnAttackCancelled() override;
	virtual void OnDefeatedServer() override;
	virtual void OnEncounterReset() override;

	/** Anneau de runes (Reliable : mécanique lisible, 3 envois par attaque ; tous les clients) : l'événement Blueprint anime l'anneau jusqu'à MaxRadius. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_RuneRing(FVector Center, float Speed, float Thickness, float MaxRadius);

	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Colossus", meta = (DisplayName = "On Rune Ring"))
	void BP_OnRuneRing(FVector Center, float Speed, float Thickness, float MaxRadius);

	/** Soin de la Résonance (Reliable : un envoi par canalisation) : faisceaux des obélisques vers le Colosse. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_ResonanceFinished(bool bCompleted, float HealAmount);

	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Colossus", meta = (DisplayName = "On Resonance Finished"))
	void BP_OnResonanceFinished(bool bCompleted, float HealAmount);

private:
	/** Anneau d'ondes runiques en propagation (données serveur). */
	struct FRuneRing
	{
		FVector Center = FVector::ZeroVector;
		double StartTime = 0.0;
		float LastRadius = 0.f;
		float MaxRadius = 0.f;
		float HalfWidth = 0.f;
		FEMBHitData HitTemplate;
		TSet<TWeakObjectPtr<AActor>> AlreadyHit;
	};

	void StartRuneRing(FGameplayTag AttackTag);
	void TickRuneRings();

	void CompleteResonance();
	void InterruptResonance();

	TWeakObjectPtr<AEMBColossusArena> Arena;

	TArray<FRuneRing> ActiveRings;
	FTimerHandle RingTimerHandle;

	FTimerHandle ResonanceTimerHandle;
	bool bChannelingResonance = false;
	float ResonancePoiseAccumulated = 0.f;
};
