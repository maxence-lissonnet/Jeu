// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBDragonTypes.h"
#include "BTTask_EMBSeraphPlunge.generated.h"

class AEMBSeraphBoss;

/**
 * Plongeons scriptés de Nyxariel, tâche latente découpée en "étapes de trajet" (legs). L'attaque est lue dans le
 * Blackboard (SelectedAttackTag), SelectedAttackKind = 10 (Plunge).
 *
 * PIQUÉ EN SPIRALE (Attack.Seraph.SpiralDive, phases 1-2)
 *   Ombre au sol 1,2 s sur la position PRÉDITE de la cible (Multicast_ShowGroundMarker), flash de 0,35 s avant l'impact.
 *   Montée préparatoire à l'aplomb de l'ombre (étape 1), puis plongée en hélice décroissante (étape 2) dont l'arrivée est
 *   calée exactement sur la fin de l'ombre : la position est une fonction du TEMPS (pas de la vitesse), l'impact tombe
 *   à l'instant annoncé quelle que soit la distance. Dégâts : sphère de rayon 400 autour du point MARQUÉ.
 *
 * CHUTE DE L'ÉTOILE MORTE (Attack.Seraph.DeadStarFall, phase 3)
 *   Trois bonds en arc, chacun terminé par un impact (rayon 400, 220 / 80) : ombre 0,8 s avant chaque impact, impacts
 *   espacés de 0,9 s (le marqueur du bond suivant apparaît 0,1 s après l'impact précédent). Chaque point d'impact est
 *   prédit au début de son bond. Nyxariel est au sol en phase 3 : le vol n'est activé que pour la durée de l'attaque
 *   (SetFlying(true) au début, SetFlying(false) à la fin ou à l'abandon). Après le 3e impact : vulnérable 4 s
 *   (BeginVulnerableWindow), il reste au sol.
 *
 * Les événements d'impact sont transmis aux ombres jumelles (BroadcastShadowEcho). Aucun timer : bNotifyTick. Le
 * nettoyage (orientation, retour au sol) est idempotent et couvre fin normale et abandon.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBSeraphPlunge : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBSeraphPlunge();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

private:
	enum class EPlungeKind : uint8
	{
		SpiralDive,
		DeadStarFall
	};

	enum class EStage : uint8
	{
		Moving,
		Recover,
		Vulnerable
	};

	void BeginSpiral(AEMBSeraphBoss& Boss);
	void BeginDeadStar(AEMBSeraphBoss& Boss);

	/** Prépare le trajet numéro LegIndex (calcule départ, arrivée, durée, marqueurs). */
	void StartLeg(AEMBSeraphBoss& Boss);
	void TickMarkers(AEMBSeraphBoss& Boss);
	void TickLeg(AEMBSeraphBoss& Boss, float DeltaSeconds);
	void FinishLeg(AEMBSeraphBoss& Boss);
	void DoImpact(AEMBSeraphBoss& Boss);
	void BeginVulnerable(AEMBSeraphBoss& Boss);
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBSeraphBoss> BossPtr;
	TWeakObjectPtr<AActor> TargetPtr;
	const FEMBDragonAttackRow* ActiveRow = nullptr;
	FGameplayTag ActiveTag;
	FEMBHitData HitTemplate;

	EPlungeKind Kind = EPlungeKind::SpiralDive;
	EStage Stage = EStage::Moving;

	float Clock = 0.f;          // temps écoulé depuis le début de la tâche
	float StageTime = 0.f;      // temps écoulé dans l'étape Recover / Vulnerable
	float ImpactRadius = 0.f;
	float MarkDuration = 0.f;   // durée d'une ombre

	// --- Trajet courant ---
	int32 LegIndex = 0;
	int32 LegCount = 0;
	float LegElapsed = 0.f;
	float LegDuration = 0.f;
	float LegArcHeight = 0.f;
	float LegHelixRadius = 0.f;
	float LegHelixTurns = 0.f;
	bool bLegHasImpact = false;
	FVector LegStart = FVector::ZeroVector;
	FVector LegEnd = FVector::ZeroVector;
	FVector ImpactPoint = FVector::ZeroVector;

	// --- Marqueurs (temps de tâche) ---
	float MarkerShowClock = 0.f;
	float FlashClock = 0.f;
	bool bMarkerShown = false;
	bool bFlashShown = false;

	// --- Piqué en spirale : préparation ---
	FVector SpiralDiveStart = FVector::ZeroVector;
	float SpiralClimbDuration = 0.f;
	float SpiralTravelTime = 0.f;

	// --- Chute de l'étoile morte ---
	float DeadStarInterval = 0.f;

	bool bCleanedUp = true;
};
