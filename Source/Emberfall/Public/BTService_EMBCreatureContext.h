// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTService.h"
#include "BTService_EMBCreatureContext.generated.h"

/**
 * Service de contexte des créatures légères (BT_Creature, voir EMBCreatureAIController.h). Tourne toutes les 0,25 s
 * (le palier "réduit" de la significance espace aussi le tick du BT) et écrit dans le Blackboard :
 *  - TargetActor / TargetDistance : meilleur joueur détecté (vue x visibilité météo, ouïe, agresseur récent) ;
 *  - bReturnHome : laisse dépassée (distance au point d'apparition > LeashRadius), levée à l'arrivée ;
 *  - bShouldFlee : feu à portée (si la créature le craint) ou PV sous le seuil de fuite avec une menace présente ;
 *  - bTargetInAttackRange : une attaque est utilisable contre la cible ;
 *  - HomeLocation / PatrolLocation : point d'apparition et prochain point de patrouille ;
 *  - bIsDead / bIsStaggered ;
 * et règle la vitesse (marche en patrouille, course en poursuite, fuite et retour).
 */
UCLASS()
class EMBERFALL_API UBTService_EMBCreatureContext : public UBTService
{
	GENERATED_BODY()

public:
	UBTService_EMBCreatureContext();

protected:
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual FString GetStaticDescription() const override;

	/** Délai maximal (s) avant de choisir un nouveau point de patrouille, même si l'actuel n'est pas atteint. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Creature", meta = (ClampMin = "1.0"))
	float PatrolRepickInterval = 20.f;

	/** Distance (cm) à laquelle le point de patrouille est considéré atteint. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Creature", meta = (ClampMin = "50.0"))
	float PatrolReachedRadius = 200.f;
};
