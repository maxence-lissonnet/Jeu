// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTTask_EMBSelectAttack.h"

#include "Emberfall.h"
#include "EMBDragonAIController.h"
#include "EMBDragonBoss.h"
#include "EMBDragonTypes.h"
#include "EMBGameplayTags.h"

#include "AIController.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	/** Facteur distance : 1 au centre de la plage, EdgeFloor aux bords, 0 hors plage. MaxRange <= 0 : illimitée. */
	float ComputeDistanceFactor(const FEMBDragonAttackRow& Row, const float Distance)
	{
		if (Distance < Row.MinRange)
		{
			return 0.f;
		}
		if (Row.MaxRange <= 0.f)
		{
			return 1.f;
		}
		if (Distance > Row.MaxRange)
		{
			return 0.f;
		}

		const float HalfSpan = (Row.MaxRange - Row.MinRange) * 0.5f;
		if (HalfSpan <= KINDA_SMALL_NUMBER)
		{
			return 1.f;
		}
		const float Center = Row.MinRange + HalfSpan;
		const float EdgeRatio = FMath::Clamp(FMath::Abs(Distance - Center) / HalfSpan, 0.f, 1.f);
		return FMath::Lerp(1.f, EMBDragonDefaults::DistanceFactorEdgeFloor, EdgeRatio);
	}

	/** Facteur angle : 0 hors cône, 1 face à la cible, EdgeFloor au bord du cône. Attaque arrière : 1 si |angle| >= seuil. */
	float ComputeAngleFactor(const FEMBDragonAttackRow& Row, const float SignedAngleDeg)
	{
		const float AbsAngle = FMath::Abs(SignedAngleDeg);
		if (Row.bRearOnly)
		{
			return AbsAngle >= Row.RearMinAngleDeg ? 1.f : 0.f;
		}
		if (Row.HalfAngleDeg >= 179.f)
		{
			return 1.f; // attaque à 360 degrés
		}
		if (AbsAngle > Row.HalfAngleDeg)
		{
			return 0.f;
		}
		return FMath::Lerp(1.f, EMBDragonDefaults::AngleFactorEdgeFloor, AbsAngle / FMath::Max(Row.HalfAngleDeg, KINDA_SMALL_NUMBER));
	}

	struct FWeightedCandidate
	{
		const FEMBDragonAttackRow* Row = nullptr;
		float Weight = 0.f;
	};
}

UBTTask_EMBSelectAttack::UBTTask_EMBSelectAttack()
{
	NodeName = TEXT("EMB Select Dragon Attack");
	// Une instance par arbre : le flux aléatoire est propre au noeud.
	bCreateNodeInstance = true;
}

EBTNodeResult::Type UBTTask_EMBSelectAttack::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	AEMBDragonBoss* Boss = AIController ? Cast<AEMBDragonBoss>(AIController->GetPawn()) : nullptr;
	UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Boss || !Blackboard || !Boss->HasAuthority() || Boss->IsDead())
	{
		return EBTNodeResult::Failed;
	}

	// Garde "en chute" : juste après un vol ou un piqué le dragon retombe ; aucune attaque ne doit partir
	// avant l'atterrissage (la branche de repli du BT prend le relais).
	if (const UCharacterMovementComponent* Move = Boss->GetCharacterMovement())
	{
		if (Move->IsFalling())
		{
			return EBTNodeResult::Failed;
		}
	}

	const AActor* Target = Cast<AActor>(Blackboard->GetValueAsObject(AEMBDragonAIController::BBKey_TargetActor));
	if (!Target)
	{
		return EBTNodeResult::Failed;
	}

	// Distance et angle recalculés ici (et non lus dans le Blackboard, rafraîchi toutes les 0,2 s) : la
	// sélection doit refléter la position actuelle du joueur.
	float Distance = 0.f;
	float SignedAngle = 0.f;
	Boss->GetDistanceAndAngleTo(Target, Distance, SignedAngle);

	const int32 Phase = Boss->GetCurrentPhase();
	const bool bCrowded = Boss->CountPlayersWithin(EMBDragonDefaults::WingBuffetCrowdRadius) >= EMBDragonDefaults::WingBuffetCrowdCount;
	const FGameplayTag WingBuffetTag = EMBTags::Attack_Dragon_WingBuffet.GetTag();

	TArray<FWeightedCandidate, TInlineAllocator<16>> Candidates;
	float TotalWeight = 0.f;

	for (const FEMBDragonAttackRow* Row : Boss->GetAttackRows())
	{
		// Exclusions : attaque scriptée (Cataclysm), interdite en phase courante, en recharge.
		if (Row->bScripted || !Row->IsAllowedInPhase(Phase) || Boss->IsAttackOnCooldown(Row->AttackTag))
		{
			continue;
		}

		const float DistanceFactor = ComputeDistanceFactor(*Row, Distance);
		const float AngleFactor = ComputeAngleFactor(*Row, SignedAngle);
		if (DistanceFactor <= 0.f || AngleFactor <= 0.f)
		{
			continue;
		}

		float Weight = Row->BaseWeight * DistanceFactor * AngleFactor * Row->GetPhaseWeight(Phase);

		// Anti-répétition : la dernière attaque est fortement pénalisée, l'avant-dernière modérément.
		if (Row->AttackTag == Boss->GetLastAttackTag())
		{
			Weight *= EMBDragonDefaults::RepeatPenaltyLast;
		}
		else if (Row->AttackTag == Boss->GetPreviousAttackTag())
		{
			Weight *= EMBDragonDefaults::RepeatPenaltyPrevious;
		}

		// Rafale d'ailes : réponse à un groupe de joueurs collés au dragon.
		if (bCrowded && Row->AttackTag == WingBuffetTag)
		{
			Weight *= EMBDragonDefaults::WingBuffetCrowdMultiplier;
		}

		if (Weight > 0.f)
		{
			FWeightedCandidate& Candidate = Candidates.AddDefaulted_GetRef();
			Candidate.Row = Row;
			Candidate.Weight = Weight;
			TotalWeight += Weight;
		}
	}

	if (Candidates.Num() == 0 || TotalWeight <= 0.f)
	{
		return EBTNodeResult::Failed; // le BT bascule sur le repositionnement
	}

	if (!bRandomSeeded)
	{
		RandomStream.GenerateNewSeed();
		bRandomSeeded = true;
	}

	// Tirage pondéré : on parcourt le cumul des poids jusqu'à dépasser la valeur tirée.
	const float Roll = RandomStream.FRandRange(0.f, TotalWeight);
	float Cumulative = 0.f;
	const FEMBDragonAttackRow* Chosen = Candidates.Last().Row; // repli : garde contre l'arrondi flottant
	for (const FWeightedCandidate& Candidate : Candidates)
	{
		Cumulative += Candidate.Weight;
		if (Roll <= Cumulative)
		{
			Chosen = Candidate.Row;
			break;
		}
	}

	Boss->RegisterAttackSelected(*Chosen);

	float Duration = 0.f;
	const bool bTimedKind = Chosen->Kind == EEMBDragonAttackKind::Melee
		|| Chosen->Kind == EEMBDragonAttackKind::Area
		|| Chosen->Kind == EEMBDragonAttackKind::Grab;
	if (bStartTimedAttacks && bTimedKind)
	{
		Duration = Boss->BeginTimedAttack(*Chosen);
	}

	Blackboard->SetValueAsName(AEMBDragonAIController::BBKey_SelectedAttackTag, Chosen->AttackTag.GetTagName());
	Blackboard->SetValueAsInt(AEMBDragonAIController::BBKey_SelectedAttackKind, static_cast<int32>(Chosen->Kind));
	Blackboard->SetValueAsFloat(AEMBDragonAIController::BBKey_SelectedAttackDuration, Duration);

	UE_LOG(LogEmberfall, Verbose, TEXT("Vaelthyrax : attaque %s (distance %.0f, angle %.0f, %d candidates)"),
		*Chosen->AttackTag.ToString(), Distance, SignedAngle, Candidates.Num());

	return EBTNodeResult::Succeeded;
}

FString UBTTask_EMBSelectAttack::GetStaticDescription() const
{
	return FString::Printf(TEXT("Tirage pondere anti-repetition (distance, angle, phase, recharges)\nLancement des attaques de melee : %s"),
		bStartTimedAttacks ? TEXT("oui") : TEXT("non"));
}
