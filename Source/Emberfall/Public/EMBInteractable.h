// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "EMBInteractable.generated.h"

class APawn;

/**
 * Interface native (non implémentable en Blueprint) des acteurs avec lesquels on interagit :
 * sac de mort, lit et feu de camp (point de réapparition), joueur mort à réanimer...
 *
 * Le personnage (AEMBPlayerCharacter) repère la cible localement (invite d'interface), puis demande
 * l'interaction au serveur, qui revérifie distance et CanInteract avant d'appeler Interact.
 */
UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UEMBInteractable : public UInterface
{
	GENERATED_BODY()
};

class EMBERFALL_API IEMBInteractable
{
	GENERATED_BODY()

public:
	/** Serveur ET client (invite d'interface) : cet instigateur peut-il interagir maintenant ? */
	virtual bool CanInteract(const APawn* Instigator) const = 0;

	/** Serveur uniquement. */
	virtual void Interact(APawn* Instigator) = 0;

	/** Texte de l'invite : « Ramasser le sac », « Se lier au lit »... */
	virtual FText GetInteractionText(const APawn* Instigator) const = 0;

	/** Durée de maintien requise en secondes (0 = interaction instantanée ; 4,0 pour la réanimation). */
	virtual float GetInteractionHoldTime() const { return 0.f; }
};
