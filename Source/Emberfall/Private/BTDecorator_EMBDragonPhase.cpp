// Copyright ADWEB Conseil. Tous droits réservés.

#include "BTDecorator_EMBDragonPhase.h"

#include "EMBDragonAIController.h"

#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"

UBTDecorator_EMBDragonPhase::UBTDecorator_EMBDragonPhase()
{
	NodeName = TEXT("EMB Dragon Phase");

	// Le décorateur observe la clé : il doit être notifié à l'entrée/sortie de pertinence pour (dés)enregistrer l'observateur.
	bNotifyBecomeRelevant = true;
	bNotifyCeaseRelevant = true;

	// Seule une clé entière est acceptée ; par défaut CurrentPhase.
	BlackboardKey.AddIntFilter(this, GET_MEMBER_NAME_CHECKED(UBTDecorator_EMBDragonPhase, BlackboardKey));
	BlackboardKey.SelectedKeyName = AEMBDragonAIController::BBKey_CurrentPhase;
}

bool UBTDecorator_EMBDragonPhase::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent();
	if (!Blackboard)
	{
		return false;
	}

	const int32 Phase = Blackboard->GetValue<UBlackboardKeyType_Int>(BlackboardKey.GetSelectedKeyID());
	return Phase >= MinPhase && Phase <= MaxPhase;
}

void UBTDecorator_EMBDragonPhase::OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent())
	{
		Blackboard->RegisterObserver(
			BlackboardKey.GetSelectedKeyID(),
			this,
			FOnBlackboardChangeNotification::CreateUObject(this, &UBTDecorator_EMBDragonPhase::OnPhaseKeyChanged));
	}
}

void UBTDecorator_EMBDragonPhase::OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	if (UBlackboardComponent* Blackboard = OwnerComp.GetBlackboardComponent())
	{
		Blackboard->UnregisterObserversFrom(this);
	}
}

EBlackboardNotificationResult UBTDecorator_EMBDragonPhase::OnPhaseKeyChanged(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID)
{
	// ConditionalFlowAbort applique le mode "Observer Aborts" configuré (Self / Lower Priority / Both) et ne
	// relance l'arbre que si le résultat de la condition a réellement changé (ConditionResultChanged).
	if (UBehaviorTreeComponent* BehaviorTreeComp = Cast<UBehaviorTreeComponent>(Blackboard.GetBrainComponent()))
	{
		ConditionalFlowAbort(*BehaviorTreeComp, EBTDecoratorAbortRequest::ConditionResultChanged);
	}
	return EBlackboardNotificationResult::ContinueObserving;
}

FString UBTDecorator_EMBDragonPhase::GetStaticDescription() const
{
	if (MinPhase == MaxPhase)
	{
		return FString::Printf(TEXT("Phase du dragon == %d"), MinPhase);
	}
	return FString::Printf(TEXT("Phase du dragon entre %d et %d"), MinPhase, MaxPhase);
}
