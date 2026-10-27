// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "EMBItemTypes.h"
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

	/**
	 * Serveur. Met en attente l'équipement et la file de fabrication d'un pawn sur le point d'être détruit
	 * (réapparition). Le PlayerState survit au pawn : les données sont réimportées à la prochaine possession,
	 * même si la création du nouveau pawn échoue puis est retentée.
	 */
	void StorePendingLoadout(const FEMBEquipmentSaveData& Equipment, const FEMBCraftingSaveData& Crafting);

	/** Serveur. Récupère et vide les données en attente ; faux s'il n'y en avait pas. */
	bool ConsumePendingLoadout(FEMBEquipmentSaveData& OutEquipment, FEMBCraftingSaveData& OutCrafting);

protected:
	/** Voyage transparent (seamless travel) : copie la progression de cet état vers le nouvel état. */
	virtual void CopyProperties(APlayerState* NewPlayerState) override;

	/** Reconnexion d'un joueur inactif : reprend la progression de l'ancien état. */
	virtual void OverrideWith(APlayerState* OldPlayerState) override;

private:
	/** Serveur seulement, non répliqué : équipement et fabrication en attente d'un nouveau pawn. */
	FEMBEquipmentSaveData PendingEquipment;
	FEMBCraftingSaveData PendingCrafting;
	bool bHasPendingLoadout = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBProgressionComponent> Progression;
};
