// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "EMBCombatTypes.h"
#include "EMBPlayerCharacter.generated.h"

class UCameraComponent;
class UEMBCharacterMovementComponent;
class UEMBBuildingComponent;
class UEMBCombatComponent;
class UEMBInventoryComponent;
class UEMBStatsComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
struct FInputActionValue;

/**
 * Personnage jouable d'Emberfall (troisième personne).
 *
 * Responsabilités : caméra, composants de jeu (jauges, combat, inventaire, construction) et traduction des
 * actions Enhanced Input en appels aux composants. Aucune règle de combat ici : le routage des coups passe
 * par UEMBCombatComponent (le personnage n'implémente donc PAS IEMBHitReceiver).
 *
 * En mode construction, l'attaque légère confirme le placement et les autres actions de combat sont suspendues.
 */
UCLASS()
class EMBERFALL_API AEMBPlayerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/** Substitue UEMBCharacterMovementComponent au composant de mouvement par défaut (sprint prédit). */
	AEMBPlayerCharacter(const FObjectInitializer& ObjectInitializer);

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void PawnClientRestart() override;

	FORCEINLINE UEMBStatsComponent* GetStatsComponent() const { return Stats; }
	FORCEINLINE UEMBCombatComponent* GetCombatComponent() const { return Combat; }
	FORCEINLINE UEMBInventoryComponent* GetInventoryComponent() const { return Inventory; }
	FORCEINLINE UEMBBuildingComponent* GetBuildingComponent() const { return Building; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Movement")
	bool IsSprinting() const;

	/**
	 * Appelé par UEMBCharacterMovementComponent sur le serveur quand le souhait de sprint du joueur change.
	 * Démarre/arrête la facturation d'endurance et refuse le sprint si la jauge est vide.
	 */
	void NotifySprintRequested(bool bRequested);

	// ---------------------------------------------------------------- Entrées (Enhanced Input), EditDefaultsOnly

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/** Priorité du contexte de mapping (plus grand = prioritaire). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	int32 MappingContextPriority;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> SprintAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> LightAttackAction;

	/** Maintenu = charge ; relâché avant 0,4 s = attaque lourde (Started/Completed). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> HeavyAttackAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> DodgeAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> ParryAction;

	/** Maintenu = garde (Started/Completed). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> BlockAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> ToggleBuildAction;

	/** Axe 1D : valeur positive = rotation horaire, négative = anti-horaire. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> RotatePieceAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> ConfirmPlaceAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> CancelAction;

	// Les vitesses de marche et de sprint se règlent sur UEMBCharacterMovementComponent (WalkSpeed / SprintSpeed).

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** Serveur : à chaque (re)possession, applique les bonus de passifs du joueur à ce pawn. */
	virtual void PossessedBy(AController* NewController) override;
	virtual bool CanJumpInternal_Implementation() const override;
	virtual void OnJumped_Implementation() override;

private:
	// Composants
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBStatsComponent> Stats;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBCombatComponent> Combat;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBInventoryComponent> Inventory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBBuildingComponent> Building;

	// Callbacks Enhanced Input
	void OnMove(const FInputActionValue& Value);
	void OnLook(const FInputActionValue& Value);
	void OnSprintStarted();
	void OnSprintCompleted();
	void OnLightAttackStarted();
	void OnHeavyAttackStarted();
	void OnHeavyAttackReleased();
	void OnDodgeStarted();
	void OnParryStarted();
	void OnBlockStarted();
	void OnBlockReleased();
	void OnToggleBuild();
	void OnRotatePiece(const FInputActionValue& Value);
	void OnConfirmPlace();
	void OnCancel();

	/** Transmet une action de combat au composant, sauf en mode construction. */
	void SendCombatInput(EEMBCombatInput Input);
	bool IsInBuildMode() const;

	UFUNCTION()
	void HandleCombatStateChanged(EEMBCombatState OldState, EEMBCombatState NewState);

	// Sprint : le souhait voyage dans les flags du mouvement (UEMBCharacterMovementComponent) ;
	// l'endurance est facturée par le serveur, qui peut forcer l'arrêt.
	UEMBCharacterMovementComponent* GetEMBMovement() const;
	void DrainSprintStamina();
	void ForceStopSprint();

	/** Client propriétaire : le serveur a interrompu le sprint (endurance vide, étourdissement). */
	UFUNCTION(Client, Reliable)
	void Client_StopSprint();

	// Charge portée -> roulade (légère / moyenne / lourde / interdite)
	UFUNCTION()
	void HandleInventoryChanged(FName ResourceId, int32 NewCount);
	void UpdateEncumbrance();

	/** Voie unique de confirmation de pose (attaque légère ou touche dédiée), avec anti-rebond. */
	void TryConfirmPlacement();

	FTimerHandle SprintDrainTimerHandle;
	double LastConfirmPlacementTime;
};
