// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "EMBDragonTypes.h"
#include "BTTask_EMBDragonBreath.generated.h"

class AEMBDragonBoss;

/**
 * Tâche latente de souffle de Vaelthyrax. Lit l'attaque dans le Blackboard (SelectedAttackTag) ou, avec
 * bUseCataclysmTag, joue le Cataclysm scripté.
 *
 * Déroulé : télégraphe (durée de la DataTable x 0,9 en phase 3) -> phase de tir par ticks (8 par seconde, un
 * ApplyConeDamage / ApplyAreaDamage par tick) -> fin. Modes :
 *  - Cone      : cône de 20 degrés, 2,5 s, orientation verrouillée à la fin du télégraphe (sortir du cône dès 1,4 s) ;
 *  - Sweep     : arc de 120 degrés en 2,0 s, rotation progressive du dragon ;
 *  - Aerial    : vol rasant en ligne droite, faisceau incliné vers le sol, zones de feu de 8 s à 20/s ;
 *  - Cataclysm : 4 s de charge en vol (non réduits en phase 3), puis embrasement total 900 dégâts avec ligne de vue.
 *
 * Aucun timer : la tâche utilise bNotifyTick (TickTask). Le nettoyage (VFX de souffle, orientation, retour au sol)
 * est centralisé et idempotent : il s'exécute sur fin normale (OnTaskFinished) comme sur abandon (AbortTask), par
 * exemple lorsque la posture est brisée ou que la phase change.
 *
 * Brûlure : 15/s pendant 4 s, appliquée par AEMBDragonBoss::ApplyBurn (un seul timer partagé côté boss) aux
 * cibles touchées par un souffle dont la ligne DataTable a bAppliesBurn.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBDragonBreath : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBDragonBreath();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** Vrai : joue Attack.Dragon.Cataclysm (branche scriptée du BT) au lieu de l'attaque du Blackboard. */
	UPROPERTY(EditAnywhere, Category = "Souffle")
	bool bUseCataclysmTag = false;

private:
	enum class EBreathStage : uint8
	{
		Telegraph,
		Firing
	};

	void ResetState();
	void LockAim(AEMBDragonBoss& Boss);
	void BeginFiring(AEMBDragonBoss& Boss);
	void DoBreathTick(AEMBDragonBoss& Boss);
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBDragonBoss> BossPtr;
	TWeakObjectPtr<AActor> TargetPtr;
	const FEMBDragonAttackRow* ActiveRow = nullptr;

	EBreathStage Stage = EBreathStage::Telegraph;
	EEMBBreathMode Mode = EEMBBreathMode::Cone;

	float StageTime = 0.f;
	float TelegraphDuration = 0.f;
	float FireDuration = 0.f;
	float AimLockTime = 0.f;
	float TickAccumulator = 0.f;
	int32 FireTickCount = 0;

	bool bNeedsFlight = false;
	bool bAimLocked = false;
	bool bCleanedUp = true;

	float ClimbTargetZ = 0.f;
	float BeamPitchDeg = 0.f;
	float SweepStartYaw = 0.f;
	float SweepSign = 1.f;

	/** Positions des zones de feu posées pendant le strafe aérien (envoyées aux clients en une fois). */
	TArray<FVector> TrailZonePositions;

	FVector StrafeEnd = FVector::ZeroVector;
	float StrafeSpeed = 0.f;
};
