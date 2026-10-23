// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_EMBSelectAttack.generated.h"

/**
 * Sélection pondérée anti-répétition de la prochaine attaque de Vaelthyrax (docs/00, section 6) :
 *
 *   score = poids de base x facteur distance x facteur angle x poids de phase x pénalité de répétition
 *
 *  - facteur distance : 1 au centre de la plage [MinRange, MaxRange], décroît linéairement vers les bords, 0 hors plage ;
 *  - facteur angle    : 1 face à la cible, décroît vers le bord du cône frontal, 0 hors cône (1 pour les attaques 360 degrés) ;
 *  - pénalité         : dernière attaque x0,25 ; avant-dernière x0,6 ;
 *  - WingBuffet x3 si au moins 2 joueurs sont à moins de 800 cm ;
 *  - exclues : attaques en recharge, attaques scriptées (Cataclysm), attaques interdites dans la phase courante.
 *
 * Le tirage est un tirage aléatoire pondéré (FRandomStream). La tâche écrit SelectedAttackTag (Name),
 * SelectedAttackKind (Int) et SelectedAttackDuration (Float) dans le Blackboard. Les attaques de mêlée, de zone et de
 * saisie sont lancées directement (impacts programmés par le boss) ; les souffles et le piqué sont exécutés par leurs
 * tâches latentes. Échoue si aucune attaque n'est valide.
 *
 * Complexité : O(N) avec N = 11 attaques, soit une quinzaine de multiplications flottantes et aucune allocation
 * (tableau à stockage inline). Appelée une fois par attaque (toutes les 2 à 4 s) : coût négligeable.
 */
UCLASS()
class EMBERFALL_API UBTTask_EMBSelectAttack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_EMBSelectAttack();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual FString GetStaticDescription() const override;

protected:
	/**
	 * Vrai : les attaques de mêlée / zone / saisie sont lancées ici (montage + impacts) et leur durée est écrite dans
	 * SelectedAttackDuration. Faux : la tâche ne fait que choisir (un autre noeud les exécute).
	 */
	UPROPERTY(EditAnywhere, Category = "Sélection")
	bool bStartTimedAttacks = true;

private:
	/** Flux aléatoire propre à cette instance de noeud (bCreateNodeInstance), semé au premier usage. */
	FRandomStream RandomStream;
	bool bRandomSeeded = false;
};
