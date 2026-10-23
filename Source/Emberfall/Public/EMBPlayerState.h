// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "EMBPlayerState.generated.h"

class UEMBProgressionComponent;

/**
 * PlayerState d'Emberfall : porte la progression du joueur. Il survit à la mort du pawn, existe sur le
 * serveur pour chaque joueur et se réplique.
 */
UCLASS()
class EMBERFALL_API AEMBPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AEMBPlayerState();

	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	UEMBProgressionComponent* GetProgressionComponent() const { return Progression; }

protected:
	/** Voyage transparent (seamless travel) : copie la progression de cet état vers le nouvel état. */
	virtual void CopyProperties(APlayerState* NewPlayerState) override;

	/** Reconnexion d'un joueur inactif : reprend la progression de l'ancien état. */
	virtual void OverrideWith(APlayerState* OldPlayerState) override;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBProgressionComponent> Progression;
};
