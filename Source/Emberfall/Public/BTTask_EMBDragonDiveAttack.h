// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "EMBDragonTypes.h"
#include "BTTask_EMBDragonDiveAttack.generated.h"

class AEMBDragonBoss;

/**
 * Piqué de Vaelthyrax, tâche latente en quatre étapes (docs/00, section 6) :
 *
 *   Takeoff   (2,0 s)  : décollage vertical jusqu'à l'altitude de croisière ;
 *   Mark      (1,5 s)  : ombre au sol (Multicast_ShowGroundMarker) sur la position PRÉDITE de la cible ; flash final
 *                        de 0,35 s avant l'impact. Le lancement du piqué est calé pour que l'impact tombe à la fin
 *                        exacte des 1,5 s, quelle que soit la distance ;
 *   Dive      (4 500 cm/s) : plongée en ligne droite vers le point marqué ;
 *   Impact    : dégâts de zone (rayon 600, 400 dégâts, 120 de poise), retour au sol, puis
 *   Vulnerable (2,5 s) : le dragon reste au sol, exposé (fenêtre de punition).
 *
 * Les durées de Takeoff et de Mark sont multipliées par 0,9 en phase 3 (télégraphes enragés). Aucun timer :
 * bNotifyTick. Le nettoyage (orientation, retour au sol) est idempotent et couvre la fin normale comme l'abandon
 * (posture brisée, changement de phase, mort).
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBDragonDiveAttack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBDragonDiveAttack();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

private:
	enum class EDiveStage : uint8
	{
		Takeoff,
		Mark,
		Dive,
		Vulnerable
	};

	void BeginMark(AEMBDragonBoss& Boss);
	void TickMarkFlash(AEMBDragonBoss& Boss);
	void DoImpact(AEMBDragonBoss& Boss);
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBDragonBoss> BossPtr;
	TWeakObjectPtr<AActor> TargetPtr;
	const FEMBDragonAttackRow* ActiveRow = nullptr;

	EDiveStage Stage = EDiveStage::Takeoff;

	float StageTime = 0.f;
	float TakeoffDuration = 0.f;
	float MarkDuration = 0.f;
	float MarkClock = 0.f;
	float LaunchTime = 0.f;
	float DiveElapsed = 0.f;
	float ImpactRadius = 0.f;
	float StartZ = 0.f;
	float CruiseZ = 0.f;

	FVector MarkerLocation = FVector::ZeroVector;
	FVector DiveDestination = FVector::ZeroVector;

	bool bFlashSent = false;
	bool bCleanedUp = true;
};
