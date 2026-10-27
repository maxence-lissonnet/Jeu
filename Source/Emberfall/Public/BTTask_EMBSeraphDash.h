// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Engine/EngineTypes.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBDragonTypes.h"
#include "BTTask_EMBSeraphDash.generated.h"

class AEMBSeraphBoss;

/**
 * Dash balayé de Nyxariel : Estoc fulgurant (Attack.Seraph.Thrust) et Feinte (Attack.Seraph.Feint). Tâche latente en
 * deux étapes ; l'attaque est lue dans le Blackboard (SelectedAttackTag), SelectedAttackKind = 9 (Dash).
 *
 *   Telegraph : Estoc 0,5 s (lance qui recule) ; Feinte 1,0 s (lance levée) + délai aléatoire de 0,3 à 0,9 s tiré par
 *               le FRandomStream serveur du boss. Nyxariel suit sa cible du regard ; 0,15 s avant le dash la visée est
 *               figée (point visé mémorisé, orientation verrouillée) et l'éclat de la pointe est émis
 *               (Multicast_TipFlash) : c'est le signal d'esquive, surtout pour la Feinte ;
 *   Dash      : 1 800 cm en 0,25 s (7 200 cm/s) le long de la visée, balayé contre le décor. En vol : trajectoire
 *               3D vers le point visé (pente bornée), arrêt sur le premier obstacle ou joueur touché ; au sol (phase 3) :
 *               course à plat (AEMBBossBase::StepGroundCharge, les joueurs ne bloquent pas). Le contact est testé sur le
 *               SEGMENT parcouru à chaque image (AEMBSeraphBoss::ResolveDashContact) : un joueur n'est touché qu'une seule
 *               fois et un pas de 120 cm ne peut pas le sauter. Un coup paré arrête immédiatement le dash.
 *
 * L'événement du dash est transmis aux ombres jumelles (BroadcastShadowEcho). Aucun timer : bNotifyTick. Le
 * nettoyage (orientation, vitesse) est idempotent et couvre fin normale et abandon (étourdissement, transition, mort).
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBSeraphDash : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBSeraphDash();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual FString GetStaticDescription() const override;

private:
	enum class EStage : uint8
	{
		Telegraph,
		Dash
	};

	void LockAim(AEMBSeraphBoss& Boss);
	void BeginDash(AEMBSeraphBoss& Boss);
	void Cleanup();

	// --- État d'exécution (une instance de noeud par arbre : bCreateNodeInstance) ---
	TWeakObjectPtr<AEMBSeraphBoss> BossPtr;
	TWeakObjectPtr<AActor> TargetPtr;
	const FEMBDragonAttackRow* ActiveRow = nullptr;
	FGameplayTag ActiveTag;
	FEMBHitData HitTemplate;

	EStage Stage = EStage::Telegraph;

	float StageTime = 0.f;
	float PreDashTime = 0.f;
	float AimLockTime = 0.f;
	float DashSpeed = 0.f;
	float TravelledDistance = 0.f;
	float ContactRadius = 0.f;

	FVector AimPoint = FVector::ZeroVector;
	FVector DashDirection = FVector::ForwardVector;
	FVector DashEndLocation = FVector::ZeroVector;
	TSet<TWeakObjectPtr<AActor>> AlreadyHit;

	bool bAimLocked = false;
	bool bGroundDash = false;
	bool bParried = false;
	// Dash en vol : la capsule du boss traverse les joueurs (Overlap) le temps du dash, comme au sol.
	ECollisionResponse SavedPawnResponse = ECR_Block;
	bool bPawnResponseOverridden = false;
	bool bCleanedUp = true;
};
