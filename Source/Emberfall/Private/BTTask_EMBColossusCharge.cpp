// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBColossusCharge.h"

#include "EMBBossAIController.h"
#include "EMBBossTags.h"
#include "EMBColossusBoss.h"

#include "AIController.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

UBTTask_EMBColossusCharge::UBTTask_EMBColossusCharge()
{
	NodeName = TEXT("EMB Colossus Ram Charge");

	// Une instance par arbre (état propre à l'exécution), tick activé, notification de fin pour le nettoyage.
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_EMBColossusCharge::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBColossusBoss* Boss = AIController ? Cast<AEMBColossusBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	const FGameplayTag ChargeTag = EMBBossTags::Attack_Guardian_RamCharge.GetTag();
	const FEMBDragonAttackRow* Row = Boss->FindAttackRow(ChargeTag);
	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBBossAIController::BBKey_TargetActor));
	if (!Row || !Target)
	{
		return EBTNodeResult::Failed;
	}

	// --- Initialisation (aucun effet de bord avant ce point) ---
	// Annule d'abord toute attaque programmée par le boss encore en cours.
	Boss->CancelTimedAttack(true);

	BossPtr = Boss;
	ActiveRow = Row;
	bCleanedUp = false;
	bDirectionLocked = false;
	Stage = EChargeStage::Telegraph;
	StageTime = 0.f;
	TravelledDistance = 0.f;
	AlreadyHit.Reset();

	const float TelegraphMultiplier = Boss->GetTelegraphMultiplier();
	TelegraphDuration = Row->TelegraphTime * TelegraphMultiplier;
	// La direction est figée au début de la fenêtre d'esquive latérale (1,30 s sur 1,40 s de télégraphe).
	DirectionLockTime = FMath::Min(Row->DodgeWindowStart > 0.f ? Row->DodgeWindowStart * TelegraphMultiplier : TelegraphDuration * 0.9f, TelegraphDuration);
	MaxTravelDistance = Row->MaxRange + EMBColossusDefaults::ChargeOvershoot;
	ContactRadius = EMBColossusDefaults::ChargeContactMargin;
	if (const UCapsuleComponent* Capsule = Boss->GetCapsuleComponent())
	{
		ContactRadius += Capsule->GetScaledCapsuleRadius();
	}

	Boss->Multicast_Telegraph(ChargeTag, TelegraphDuration);
	if (UAnimMontage* Montage = Boss->ResolveMontage(Row->Montage))
	{
		Boss->PlayAttackMontage(Montage, Boss->GetAttackPlayRate());
	}
	Boss->SetFacingLocked(false); // suit la cible jusqu'au verrouillage de la direction

	return EBTNodeResult::InProgress;
}

void UBTTask_EMBColossusCharge::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBColossusBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || !ActiveRow)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	StageTime += DeltaSeconds;

	if (Stage == EChargeStage::Telegraph)
	{
		if (!bDirectionLocked && StageTime >= DirectionLockTime)
		{
			// Direction figée = direction réellement visible du Colosse (axe avant à plat) : ce que le joueur lit est
			// ce qui se produit.
			bDirectionLocked = true;
			ChargeDirection = Boss->GetActorForwardVector().GetSafeNormal2D();
			Boss->SetFacingLocked(true);
		}
		if (StageTime >= TelegraphDuration)
		{
			BeginCharging(*Boss);
		}
		return;
	}

	// --- Course ---
	const FVector Before = Boss->GetActorLocation();
	FHitResult BlockingHit;
	const bool bBlocked = Boss->StepGroundCharge(ChargeDirection, EMBColossusDefaults::ChargeSpeed, DeltaSeconds, BlockingHit);
	TravelledDistance += static_cast<float>(FVector::Dist2D(Before, Boss->GetActorLocation()));

	// Joueurs traversés par la course : touchés une seule fois chacun, qu'ils esquivent ou non. Le contact est testé
	// sur le SEGMENT parcouru pendant ce pas (et non sur la seule position finale) : à 2 000 cm/s un pas de frame
	// dépasse 30 cm, un joueur situé sur le trajet ne doit jamais être "sauté". On l'approche par une sphère
	// centrée au milieu du segment, de rayon contact + demi-longueur du segment (distance à plat).
	const FVector After = Boss->GetActorLocation();
	const FVector SegmentMid = (Before + After) * 0.5;
	const float HalfSegment = static_cast<float>(FVector::Dist2D(Before, After)) * 0.5f;
	Boss->ApplyAreaDamage(SegmentMid, ContactRadius + HalfSegment, Boss->MakeHitData(*ActiveRow), false, &AlreadyHit, true);

	if (bBlocked)
	{
		if (Boss->IsValidChargeObstacle(BlockingHit.GetActor()))
		{
			// Obélisque ou mur : dégâts à l'obstacle puis étourdi 3 s. L'étourdissement fait abandonner cette tâche
			// par le BT (observer aborts) ; on la termine proprement dès maintenant.
			Boss->ResolveChargeImpact(BlockingHit, *ActiveRow);
		}
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	if (TravelledDistance >= MaxTravelDistance)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded); // charge manquée : pas d'étourdissement
	}
}

void UBTTask_EMBColossusCharge::BeginCharging(AEMBColossusBoss& Boss)
{
	Stage = EChargeStage::Charging;
	StageTime = 0.f;
	if (!bDirectionLocked)
	{
		bDirectionLocked = true;
		ChargeDirection = Boss.GetActorForwardVector().GetSafeNormal2D();
		Boss.SetFacingLocked(true);
	}
}

EBTNodeResult::Type UBTTask_EMBColossusCharge::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBColossusCharge::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBColossusCharge::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (AEMBColossusBoss* Boss = BossPtr.Get())
	{
		Boss->SetFacingLocked(false);
		if (UCharacterMovementComponent* Move = Boss->GetCharacterMovement())
		{
			Move->StopMovementImmediately(); // fin de course : plus aucune vitesse résiduelle répliquée
		}
	}
	ActiveRow = nullptr;
	AlreadyHit.Reset();
}

FString UBTTask_EMBColossusCharge::GetStaticDescription() const
{
	return TEXT("Charge de belier : telegraphe 1,4 s, course en ligne, obstacle = etourdi 3 s");
}
