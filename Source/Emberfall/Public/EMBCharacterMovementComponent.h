// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EMBCharacterMovementComponent.generated.h"

/**
 * Mouvement du joueur avec sprint prédit correctement par le réseau.
 *
 * Pourquoi un composant dédié : changer MaxWalkSpeed à la main sur le client puis sur le serveur crée des
 * désaccords de position (corrections / rubber-banding) à chaque départ ou arrêt de sprint. Ici le souhait de
 * sprinter voyage dans les flags compressés de chaque mouvement client (FLAG_Custom_0) : le serveur rejoue
 * exactement la même vitesse maximale que le client, mouvement par mouvement.
 *
 * L'endurance reste une règle serveur : AEMBPlayerCharacter facture le sprint et appelle ForbidSprint()
 * (serveur) quand il faut l'interrompre ; le client propriétaire est prévenu par RPC.
 */
UCLASS()
class EMBERFALL_API UEMBCharacterMovementComponent : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	UEMBCharacterMovementComponent();

	/** Vitesse de marche (cm/s). Valeur de départ, hors référentiel : à régler en playtest. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "0"))
	float WalkSpeed;

	/** Vitesse de sprint (cm/s). Valeur de départ, hors référentiel. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Sprint", meta = (ClampMin = "0"))
	float SprintSpeed;

	/** Demande de sprint (entrée du joueur). Côté propriétaire ; transmise au serveur dans les flags du mouvement. */
	UFUNCTION(BlueprintCallable, Category = "Sprint")
	void SetWantsToSprint(bool bSprint);

	UFUNCTION(BlueprintPure, Category = "Sprint")
	bool WantsToSprint() const { return bWantsToSprint; }

	/** Sprint effectivement actif : demandé, autorisé, et au sol. */
	UFUNCTION(BlueprintPure, Category = "Sprint")
	bool IsSprintActive() const;

	/**
	 * Serveur : interdit le sprint jusqu'à ce que le joueur relâche la touche (endurance vide, étourdi, mort).
	 * L'interdiction est levée au prochain mouvement où le souhait de sprint repasse à faux.
	 */
	void ForbidSprint();

	virtual float GetMaxSpeed() const override;
	virtual void UpdateFromCompressedFlags(uint8 Flags) override;
	virtual class FNetworkPredictionData_Client* GetPredictionData_Client() const override;

	/** Souhait de sprint courant (état du mouvement en cours de simulation). */
	uint8 bWantsToSprint : 1;

private:
	/** Serveur uniquement : sprint interdit tant que la touche n'est pas relâchée. */
	uint8 bSprintForbidden : 1;

	void NotifyOwnerSprintRequest(bool bRequested) const;
};

/** Mouvement sauvegardé : porte le souhait de sprint pour le rejeu et la compression réseau. */
class FEMBSavedMove : public FSavedMove_Character
{
public:
	typedef FSavedMove_Character Super;

	FEMBSavedMove()
		: bSavedWantsToSprint(false)
	{
	}

	uint8 bSavedWantsToSprint : 1;

	virtual void Clear() override;
	virtual uint8 GetCompressedFlags() const override;
	virtual bool CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const override;
	virtual void SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData) override;
	virtual void PrepMoveFor(ACharacter* C) override;
};

/** Données de prédiction client : fabrique des FEMBSavedMove. */
class FEMBNetworkPredictionData_Client : public FNetworkPredictionData_Client_Character
{
public:
	typedef FNetworkPredictionData_Client_Character Super;

	explicit FEMBNetworkPredictionData_Client(const UCharacterMovementComponent& ClientMovement)
		: Super(ClientMovement)
	{
	}

	virtual FSavedMovePtr AllocateNewMove() override;
};
