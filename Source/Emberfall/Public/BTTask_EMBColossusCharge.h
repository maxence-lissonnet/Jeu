// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "EMBDragonTypes.h"
#include "BTTask_EMBColossusCharge.generated.h"

class AEMBColossusBoss;

/**
 * Charge de bélier du Colosse d'Ossivaire (Attack.Guardian.RamCharge), tâche latente en deux étapes :
 *
 *   Telegraph (1,4 s, x0,9 en phase 2) : le Colosse racle le sol en suivant sa cible du regard ; la direction est
 *             figée au début de la fenêtre d'esquive latérale (1,30 s) : le joueur lit la ligne de charge ;
 *   Charge    : course en ligne droite à vitesse constante (AEMBBossBase::StepGroundCharge) jusqu'à
 *             MaxRange + dépassement, ou jusqu'à un obstacle. Un joueur traversé par la course est touché une seule
 *             fois (ApplyAreaDamage avec ensemble des joueurs déjà traités). Si le Colosse percute un obélisque ou
 *             un mur : dégâts à l'obstacle, flash d'impact, puis étourdi 3 s (AEMBColossusBoss::ResolveChargeImpact).
 *
 * Aucun timer : bNotifyTick. Le nettoyage (orientation) est idempotent et couvre fin normale et abandon.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBColossusCharge : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBColossusCharge();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

private:
	enum class EChargeStage : uint8
	{
		Telegraph,
		Charging
	};

	void BeginCharging(AEMBColossusBoss& Boss);
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBColossusBoss> BossPtr;
	const FEMBDragonAttackRow* ActiveRow = nullptr;

	EChargeStage Stage = EChargeStage::Telegraph;

	float StageTime = 0.f;
	float TelegraphDuration = 0.f;
	float DirectionLockTime = 0.f;
	float TravelledDistance = 0.f;
	float MaxTravelDistance = 0.f;
	float ContactRadius = 0.f;

	FVector ChargeDirection = FVector::ForwardVector;
	TSet<TWeakObjectPtr<AActor>> AlreadyHit;

	bool bDirectionLocked = false;
	bool bCleanedUp = true;
};
