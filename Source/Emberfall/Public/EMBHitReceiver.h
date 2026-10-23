// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "EMBCombatTypes.h"
#include "EMBHitReceiver.generated.h"

/**
 * Interface C++ native (non implémentable en Blueprint) pour les acteurs qui traitent eux-mêmes un coup :
 * boss (points faibles selon l'os touché, invulnérabilité de transition de phase), objets destructibles.
 *
 * Appelée sur le SERVEUR uniquement, par UEMBCombatComponent::ApplyHitToActor (routage prioritaire).
 */
UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UEMBHitReceiver : public UInterface
{
	GENERATED_BODY()
};

class EMBERFALL_API IEMBHitReceiver
{
	GENERATED_BODY()

public:
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) = 0;
};
