// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Decorators/BTDecorator_BlackboardBase.h"
#include "BTDecorator_EMBDragonPhase.generated.h"

/**
 * Décorateur de phase de Vaelthyrax : vrai si MinPhase <= phase courante <= MaxPhase.
 *
 * La phase est lue dans la clé Blackboard entière CurrentPhase (poussée par le boss à chaque changement de phase).
 * Le décorateur observe cette clé : avec "Observer Aborts" (Self / Lower Priority / Both) réglé dans l'éditeur,
 * l'arbre ré-évalue la branche dès que la phase change, sans attendre un cycle du service.
 *
 * Exemples : branche réservée à la phase 3 (Min 3, Max 3) ; branche "vol fréquent" des phases 2 et 3 (Min 2, Max 3).
 */
UCLASS()
class EMBERFALL_API UBTDecorator_EMBDragonPhase : public UBTDecorator_BlackboardBase
{
	GENERATED_BODY()

public:
	UBTDecorator_EMBDragonPhase();

	virtual FString GetStaticDescription() const override;

protected:
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;
	virtual void OnBecomeRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnCeaseRelevant(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

	/** Phase minimale incluse (1 à 3). */
	UPROPERTY(EditAnywhere, Category = "Phase", meta = (ClampMin = "1", ClampMax = "3"))
	int32 MinPhase = 1;

	/** Phase maximale incluse (1 à 3). */
	UPROPERTY(EditAnywhere, Category = "Phase", meta = (ClampMin = "1", ClampMax = "3"))
	int32 MaxPhase = 3;

private:
	/** Appelé par le Blackboard quand CurrentPhase change : demande une ré-évaluation de l'arbre. */
	EBlackboardNotificationResult OnPhaseKeyChanged(const UBlackboardComponent& Blackboard, FBlackboard::FKey ChangedKeyID);
};
