// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTService.h"
#include "BTService_EMBUpdateCombatContext.generated.h"

/**
 * Met à jour le contexte de combat de Vaelthyrax dans le Blackboard :
 * TargetActor, TargetDistance, TargetAngle (signé, > 0 = cible à droite), CurrentPhase, bIsFlying, ThreatCount,
 * bCataclysmDue.
 *
 * Pourquoi un service à intervalle (0,2 s +/- 0,05) plutôt qu'un Tick :
 *  - le Behavior Tree n'a pas besoin d'une précision à la frame : 5 mises à jour par seconde suffisent pour décider
 *    d'une attaque dont le télégraphe dure plus de 0,7 s ;
 *  - l'intervalle est géré par le BT et suspendu quand la branche porteuse n'est pas active ;
 *  - la déviation aléatoire (+/- 0,05 s) évite que plusieurs boss ou services s'alignent sur la même frame ;
 *  - la désignation de cible évalue au plus 4 joueurs : coût négligeable à cette cadence.
 * Les états critiques (étourdi, transition, mort) ne passent pas par ce service : le boss les pousse
 * immédiatement dans le Blackboard (AEMBDragonAIController::SyncBlackboardState).
 *
 * Choix de la cible : le joueur le plus "menaçant" = le plus proche, pondéré par les dégâts récemment infligés au
 * boss (menace à décroissance exponentielle). La cible courante bénéficie d'un bonus d'hystérésis pour éviter les
 * changements de cible à chaque mise à jour.
 */
UCLASS()
class EMBERFALL_API UBTService_EMBUpdateCombatContext : public UBTService
{
	GENERATED_BODY()

public:
	UBTService_EMBUpdateCombatContext();

	virtual FString GetStaticDescription() const override;

protected:
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

	/** Portée maximale de désignation d'une cible (cm). Défaut : rayon de perception du dragon. */
	UPROPERTY(EditAnywhere, Category = "Contexte", meta = (ClampMin = "100.0"))
	float MaxTargetDistance = 8000.f;

	/** Bonus multiplicatif du score de la cible actuelle (hystérésis, 1,0 = aucun). */
	UPROPERTY(EditAnywhere, Category = "Contexte", meta = (ClampMin = "1.0"))
	float CurrentTargetBonus = 1.15f;
};
