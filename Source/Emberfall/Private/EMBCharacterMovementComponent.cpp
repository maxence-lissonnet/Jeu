// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCharacterMovementComponent.h"
#include "EMBPlayerCharacter.h"
#include "GameFramework/Character.h"

UEMBCharacterMovementComponent::UEMBCharacterMovementComponent()
{
	WalkSpeed = 450.f;
	SprintSpeed = 700.f;
	bWantsToSprint = false;
	bSprintForbidden = false;

	MaxWalkSpeed = WalkSpeed;
}

bool UEMBCharacterMovementComponent::IsSprintActive() const
{
	return bWantsToSprint && !bSprintForbidden && IsMovingOnGround();
}

float UEMBCharacterMovementComponent::GetMaxSpeed() const
{
	// Appelé à CHAQUE mouvement simulé, par le client (prédiction) et par le serveur (rejeu) : les deux
	// calculent la même vitesse à partir du même souhait de sprint transporté dans les flags du mouvement.
	const float BaseSpeed = Super::GetMaxSpeed();
	if (MovementMode == MOVE_Walking || MovementMode == MOVE_NavWalking)
	{
		if (!IsCrouching() && IsSprintActive())
		{
			return SprintSpeed;
		}
		// Accroupi : la vitesse réduite du moteur ; sinon WalkSpeed, qui peut donc dépasser MaxWalkSpeed.
		return IsCrouching() ? BaseSpeed : WalkSpeed;
	}
	return BaseSpeed;
}

void UEMBCharacterMovementComponent::SetWantsToSprint(bool bSprint)
{
	if (static_cast<bool>(bWantsToSprint) == bSprint)
	{
		return;
	}
	bWantsToSprint = bSprint;

	if (!bSprint)
	{
		bSprintForbidden = false; // Touche relâchée : l'interdiction éventuelle est levée.
	}
	// Sur l'autorité locale (serveur, ou hôte d'une partie listen) on prévient le personnage tout de suite.
	if (CharacterOwner && CharacterOwner->HasAuthority())
	{
		NotifyOwnerSprintRequest(bSprint);
	}
}

void UEMBCharacterMovementComponent::ForbidSprint()
{
	// N'interdit que si un sprint est réellement demandé : sinon l'interdiction resterait posée sans
	// que la touche soit relâchée (aucun front descendant pour la lever) et bloquerait le prochain sprint.
	if (CharacterOwner && CharacterOwner->HasAuthority() && bWantsToSprint)
	{
		bSprintForbidden = true;
	}
}

void UEMBCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);

	// Serveur : reconstruit le souhait de sprint du client à partir du flag du mouvement reçu.
	const bool bNewWantsToSprint = (Flags & FSavedMove_Character::FLAG_Custom_0) != 0;
	if (bNewWantsToSprint != static_cast<bool>(bWantsToSprint))
	{
		bWantsToSprint = bNewWantsToSprint;
		if (!bNewWantsToSprint)
		{
			bSprintForbidden = false;
		}
		if (CharacterOwner && CharacterOwner->HasAuthority())
		{
			NotifyOwnerSprintRequest(bNewWantsToSprint);
		}
	}
}

void UEMBCharacterMovementComponent::NotifyOwnerSprintRequest(bool bRequested) const
{
	if (AEMBPlayerCharacter* Player = Cast<AEMBPlayerCharacter>(CharacterOwner))
	{
		Player->NotifySprintRequested(bRequested);
	}
}

FNetworkPredictionData_Client* UEMBCharacterMovementComponent::GetPredictionData_Client() const
{
	check(PawnOwner != nullptr);

	if (ClientPredictionData == nullptr)
	{
		// Création paresseuse, comme le fait la classe de base ; le const_cast est le schéma standard du moteur.
		UEMBCharacterMovementComponent* MutableThis = const_cast<UEMBCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FEMBNetworkPredictionData_Client(*this);
		MutableThis->ClientPredictionData->MaxSmoothNetUpdateDist = 92.f;
		MutableThis->ClientPredictionData->NoSmoothNetUpdateDist = 140.f;
	}
	return ClientPredictionData;
}

// ------------------------------------------------------------------------------------------------
// Mouvement sauvegardé
// ------------------------------------------------------------------------------------------------

void FEMBSavedMove::Clear()
{
	Super::Clear();
	bSavedWantsToSprint = false;
}

uint8 FEMBSavedMove::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();
	if (bSavedWantsToSprint)
	{
		Result |= FLAG_Custom_0;
	}
	return Result;
}

bool FEMBSavedMove::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	// Deux mouvements dont le souhait de sprint diffère ne doivent pas être fusionnés (bande passante).
	const FEMBSavedMove* Other = static_cast<const FEMBSavedMove*>(NewMove.Get());
	if (bSavedWantsToSprint != Other->bSavedWantsToSprint)
	{
		return false;
	}
	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

void FEMBSavedMove::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);

	if (const UEMBCharacterMovementComponent* Movement = Cast<UEMBCharacterMovementComponent>(C->GetCharacterMovement()))
	{
		bSavedWantsToSprint = Movement->bWantsToSprint;
	}
}

void FEMBSavedMove::PrepMoveFor(ACharacter* C)
{
	Super::PrepMoveFor(C);

	// Rejeu d'un mouvement (correction réseau) : on restaure le souhait de sprint qu'il portait.
	if (UEMBCharacterMovementComponent* Movement = Cast<UEMBCharacterMovementComponent>(C->GetCharacterMovement()))
	{
		Movement->bWantsToSprint = bSavedWantsToSprint;
	}
}

FSavedMovePtr FEMBNetworkPredictionData_Client::AllocateNewMove()
{
	return MakeShared<FEMBSavedMove>();
}
