// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBSeraphDash.h"

#include "EMBBossAIController.h"
#include "EMBBossTags.h"
#include "EMBSeraphBoss.h"

#include "AIController.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	/** Garde-fou : durée maximale de l'étape de dash, en multiple de la durée nominale (blocage inattendu). */
	constexpr float DashTimeoutFactor = 2.f;

	/** Marge sur la distance parcourue pour considérer le dash terminé (cm). */
	constexpr float DashDistanceTolerance = 1.f;
}

UBTTask_EMBSeraphDash::UBTTask_EMBSeraphDash()
{
	NodeName = TEXT("EMB Seraph Dash");

	// Une instance par arbre (état propre à l'exécution), tick activé, notification de fin pour le nettoyage.
	bCreateNodeInstance = true;
	bNotifyTick = true;
	bNotifyTaskFinished = true;
}

EBTNodeResult::Type UBTTask_EMBSeraphDash::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBSeraphBoss* Boss = AIController ? Cast<AEMBSeraphBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Blackboard->GetValueAsName(AEMBBossAIController::BBKey_SelectedAttackTag), false);
	const bool bIsThrust = Tag == EMBBossTags::Attack_Seraph_Thrust.GetTag();
	const bool bIsFeint = Tag == EMBBossTags::Attack_Seraph_Feint.GetTag();
	const FEMBDragonAttackRow* Row = (bIsThrust || bIsFeint) ? Boss->FindAttackRow(Tag) : nullptr;
	AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBBossAIController::BBKey_TargetActor));
	if (!Row || !Target)
	{
		return EBTNodeResult::Failed;
	}

	// --- Initialisation (aucun effet de bord avant ce point) ---
	// Annule d'abord toute attaque programmée par le boss encore en cours.
	Boss->CancelTimedAttack(true);

	BossPtr = Boss;
	TargetPtr = Target;
	ActiveRow = Row;
	ActiveTag = Tag;
	bCleanedUp = false;
	bAimLocked = false;
	bParried = false;
	Stage = EStage::Telegraph;
	StageTime = 0.f;
	TravelledDistance = 0.f;
	AlreadyHit.Reset();
	HitTemplate = Boss->MakeSeraphHitData(*Row);

	const float TelegraphMultiplier = Boss->GetTelegraphMultiplier();
	PreDashTime = Row->TelegraphTime * TelegraphMultiplier;
	if (bIsFeint)
	{
		// Feinte : la levée dure TelegraphTime, puis le délai aléatoire s'ajoute avant l'éclat et le dash.
		PreDashTime += Boss->RollFeintDelay() * TelegraphMultiplier;
	}
	AimLockTime = FMath::Max(0.f, PreDashTime - EMBSeraphDefaults::DashAimLockLead);

	const float DashDuration = Row->ActiveDuration > 0.f ? Row->ActiveDuration : EMBSeraphDefaults::DashDuration;
	DashSpeed = EMBSeraphDefaults::DashDistance / FMath::Max(DashDuration, KINDA_SMALL_NUMBER);
	ContactRadius = Row->EffectRadius > 0.f ? Row->EffectRadius : EMBSeraphDefaults::DashContactRadius;

	Boss->Multicast_Telegraph(Tag, PreDashTime);
	if (UAnimMontage* Montage = Boss->ResolveMontage(Row->Montage))
	{
		Boss->PlayAttackMontage(Montage, Boss->GetAttackPlayRate());
	}
	Boss->SetFacingLocked(false); // suit la cible jusqu'au verrouillage de la visée

	return EBTNodeResult::InProgress;
}

void UBTTask_EMBSeraphDash::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, const float DeltaSeconds)
{
	AEMBSeraphBoss* Boss = BossPtr.Get();
	if (!Boss || Boss->IsDead() || Boss->IsStunned() || Boss->IsTransitioning() || !ActiveRow)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	StageTime += DeltaSeconds;

	if (Stage == EStage::Telegraph)
	{
		if (!bAimLocked && StageTime >= AimLockTime)
		{
			LockAim(*Boss);
		}
		if (StageTime >= PreDashTime)
		{
			BeginDash(*Boss);
		}
		return;
	}

	// --- Dash ---
	const FVector Before = Boss->GetActorLocation();
	bool bBlocked = false;
	if (bGroundDash)
	{
		FHitResult BlockingHit;
		bBlocked = Boss->StepGroundCharge(DashDirection, DashSpeed, DeltaSeconds, BlockingHit);
	}
	else
	{
		Boss->StepFlightToward(DashEndLocation, DashSpeed, DeltaSeconds, bBlocked);
	}
	const FVector After = Boss->GetActorLocation();
	TravelledDistance += static_cast<float>(FVector::Dist(Before, After));

	// Contact sur le segment parcouru dans cette image. Une parade parfaite arrête le dash (la lance est repoussée).
	if (Boss->ResolveDashContact(Before, After, ContactRadius, HitTemplate, AlreadyHit))
	{
		bParried = true;
	}

	const float DashDuration = EMBSeraphDefaults::DashDistance / FMath::Max(DashSpeed, KINDA_SMALL_NUMBER);
	if (bParried || bBlocked || TravelledDistance >= EMBSeraphDefaults::DashDistance - DashDistanceTolerance
		|| StageTime > DashDuration * DashTimeoutFactor)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

void UBTTask_EMBSeraphDash::LockAim(AEMBSeraphBoss& Boss)
{
	bAimLocked = true;

	// Point visé = position de la cible à cet instant (l'orientation du boss est figée avec lui) : ce que le joueur lit
	// est ce qui se produit. L'éclat de la pointe est le signal d'esquive.
	const AActor* Target = TargetPtr.Get();
	AimPoint = Target ? Target->GetActorLocation() : Boss.GetActorLocation() + Boss.GetActorForwardVector() * EMBSeraphDefaults::DashDistance;
	Boss.SetFacingLocked(true);
	Boss.Multicast_TipFlash(ActiveTag, FMath::Max(PreDashTime - StageTime, 0.f));
}

void UBTTask_EMBSeraphDash::BeginDash(AEMBSeraphBoss& Boss)
{
	if (!bAimLocked)
	{
		LockAim(Boss);
	}

	Stage = EStage::Dash;
	StageTime = 0.f;
	TravelledDistance = 0.f;
	bGroundDash = !Boss.IsFlying();

	// En vol, la capsule est bloquée par celle des joueurs : on la passe en Overlap pour traverser (même comportement
	// qu'au sol, où StepGroundCharge ignore les pawns). Restauré par Cleanup.
	if (!bGroundDash)
	{
		if (UCapsuleComponent* Capsule = Boss.GetCapsuleComponent())
		{
			SavedPawnResponse = Capsule->GetCollisionResponseToChannel(ECC_Pawn);
			Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
			bPawnResponseOverridden = true;
		}
	}

	const FVector Start = Boss.GetActorLocation();
	FVector Direction = (AimPoint - Start).GetSafeNormal();
	if (Direction.IsNearlyZero())
	{
		Direction = Boss.GetActorForwardVector();
	}
	if (bGroundDash)
	{
		Direction = Direction.GetSafeNormal2D();
	}
	else
	{
		// En vol : pente bornée (un estoc quasi vertical serait illisible et planterait Nyxariel dans le sol).
		Direction.Z = FMath::Clamp<double>(Direction.Z, -EMBSeraphDefaults::DashMaxDescentSlope, EMBSeraphDefaults::DashMaxAscentSlope);
		Direction = Direction.GetSafeNormal();
	}
	DashDirection = Direction;
	DashEndLocation = Start + Direction * EMBSeraphDefaults::DashDistance;

	// Les ombres jumelles rejouent le dash 0,3 s plus tard, vers le même point visé.
	FEMBSeraphEcho Echo;
	Echo.AttackTag = ActiveTag;
	Echo.AimPoint = AimPoint;
	Boss.BroadcastShadowEcho(Echo);
}

EBTNodeResult::Type UBTTask_EMBSeraphDash::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	Cleanup();
	return EBTNodeResult::Aborted;
}

void UBTTask_EMBSeraphDash::OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult)
{
	Cleanup();
	Super::OnTaskFinished(OwnerComp, NodeMemory, TaskResult);
}

void UBTTask_EMBSeraphDash::Cleanup()
{
	if (bCleanedUp)
	{
		return;
	}
	bCleanedUp = true;

	if (AEMBSeraphBoss* Boss = BossPtr.Get())
	{
		if (bPawnResponseOverridden)
		{
			if (UCapsuleComponent* Capsule = Boss->GetCapsuleComponent())
			{
				Capsule->SetCollisionResponseToChannel(ECC_Pawn, SavedPawnResponse);
			}
			bPawnResponseOverridden = false;
		}
		Boss->SetFacingLocked(false);
		if (UCharacterMovementComponent* Move = Boss->GetCharacterMovement())
		{
			Move->StopMovementImmediately(); // fin de dash : plus aucune vitesse résiduelle répliquée
		}
	}
	ActiveRow = nullptr;
	HitTemplate = FEMBHitData();
	AlreadyHit.Reset();
}

FString UBTTask_EMBSeraphDash::GetStaticDescription() const
{
	return TEXT("Dash de Nyxariel (Estoc / Feinte) : telegraphe, visee figee + eclat, dash 1 800 cm en 0,25 s");
}
