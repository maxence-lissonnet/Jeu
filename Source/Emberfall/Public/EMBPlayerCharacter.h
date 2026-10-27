// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayTagAssetInterface.h"
#include "EMBCombatTypes.h"
#include "EMBInteractable.h"
#include "EMBPlayerCharacter.generated.h"

class UCameraComponent;
class UEMBCharacterMovementComponent;
class UEMBBuildingComponent;
class UEMBCombatComponent;
class UEMBInventoryComponent;
class UEMBStatsComponent;
class UEMBSurvivalComponent;
class UInputAction;
class UInputMappingContext;
class USpringArmComponent;
struct FInputActionValue;

/** Cible d'interaction courante (nullptr = aucune) et texte de l'invite d'interface. Client local. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnInteractionTargetChanged, AActor*, Target, FText, Prompt);
/** Le joueur vient de mourir (true) ou d'être réanimé / de réapparaître (false). Serveur et clients. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnDeadAwaitingRespawnChanged, bool, bDead);
/** Fin d'une interaction maintenue : terminée (true) ou interrompue (false). Client propriétaire. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnInteractionHoldEnded, bool, bCompleted);

/**
 * Personnage jouable d'Emberfall (troisième personne).
 *
 * Responsabilités : caméra, composants de jeu (jauges, combat, inventaire, construction), traduction des
 * actions Enhanced Input en appels aux composants, interaction avec le monde, mort et réanimation.
 * Aucune règle de combat ici : le routage des coups passe par UEMBCombatComponent (le personnage
 * n'implémente donc PAS IEMBHitReceiver).
 *
 * En mode construction, l'attaque légère confirme le placement et les autres actions de combat sont suspendues.
 *
 * MORT : à la mort (état Dead du combat), le joueur passe en "attente de réapparition" (répliqué). Pas de
 * ragdoll : le montage de mort tient la pose finale, ce qui garde la capsule et le mesh cohérents pour une
 * réanimation (un ragdoll détacherait le mesh de la capsule et se répliquerait mal). Réapparition automatique
 * après 30 s, demandable par le joueur après 3 s ; un allié peut le réanimer en maintenant Interagir 4 s.
 *
 * ASSETS D'ENTRÉE À CRÉER (Enhanced Input, à assigner dans le Blueprint du personnage et à lier dans le
 * contexte de mapping) :
 *  - IA_Interact (booléen) : InteractAction. Utilise les événements Started et Completed (maintien) ;
 *    ne pas mettre de déclencheur "Hold" : la durée de maintien est gérée par le serveur. Quand le joueur
 *    est mort, la même touche demande la réapparition.
 *  - IA_Respawn (booléen, optionnel) : RespawnAction, une touche dédiée à la réapparition.
 */
UCLASS()
class EMBERFALL_API AEMBPlayerCharacter : public ACharacter, public IEMBInteractable, public IGameplayTagAssetInterface
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
	FORCEINLINE UEMBSurvivalComponent* GetSurvivalComponent() const { return Survival; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Movement")
	bool IsSprinting() const;

	/**
	 * Appelé par UEMBCharacterMovementComponent sur le serveur quand le souhait de sprint du joueur change.
	 * Démarre/arrête la facturation d'endurance et refuse le sprint si la jauge est vide.
	 */
	void NotifySprintRequested(bool bRequested);

	// ---------------------------------------------------------------- Mort et réapparition

	/** Client -> serveur. Refusé avant 3,0 s ; ensuite accepté (voir bRespawnRequiresNoLivingAlly pour restreindre la coop). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Death")
	void RequestRespawn();

	UFUNCTION(BlueprintPure, Category = "Emberfall|Death")
	bool IsDeadAwaitingRespawn() const { return bDeadAwaitingRespawn; }

	/**
	 * Serveur : la mort a eu lieu en zone létale (coup fatal marqué bLethalZone : lave, vide ; ou chute hors du monde).
	 * Le sac est alors déposé au dernier point sûr plutôt qu'à l'endroit de la mort, souvent inaccessible.
	 */
	bool WasKilledInLethalZone() const { return bKilledInLethalZone; }

	/** Hors du monde (KillZ) : tue par le flux normal au lieu de détruire le pawn, pour permettre le sac et la réapparition. */
	virtual void FellOutOfWorld(const UDamageType& DamageType) override;

	/** Secondes restantes avant la réapparition automatique (0 si vivant). Estimation client via l'horloge serveur. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Death")
	float GetAutoRespawnTimeRemaining() const;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Death")
	FEMBOnDeadAwaitingRespawnChanged OnDeadAwaitingRespawnChanged;

	// ---------------------------------------------------------------- Interaction

	/** Cible d'interaction actuelle (client local), pour l'invite d'interface. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Interaction")
	AActor* GetInteractionTarget() const { return InteractionTarget.Get(); }

	/** Avancement 0..1 d'un maintien en cours (estimation locale ; 0 sans maintien). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Interaction")
	float GetInteractionHoldProgress() const;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Interaction")
	FEMBOnInteractionTargetChanged OnInteractionTargetChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Interaction")
	FEMBOnInteractionHoldEnded OnInteractionHoldEnded;

	// IEMBInteractable : un joueur mort en attente se réanime (maintien de 4 s par un allié vivant).
	virtual bool CanInteract(const APawn* InInstigator) const override;
	virtual void Interact(APawn* InInstigator) override;
	virtual FText GetInteractionText(const APawn* InInstigator) const override;
	virtual float GetInteractionHoldTime() const override { return EMBCombatDefaults::ReviveHoldTime; }

	// IGameplayTagAssetInterface : tags d'état (invulnérable, étourdi, mort, en attente de réapparition).
	virtual void GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Faux (défaut, arbitrage du chef) : tout joueur mort peut réapparaître dès 3 s. Vrai : en coop, attendre l'échéance automatique tant qu'un allié vivant peut réanimer. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Death")
	bool bRespawnRequiresNoLivingAlly;

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

	/** Interagir (Started / Completed pour le maintien). Mort : demande la réapparition. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> InteractAction;

	/** Touche dédiée à la réapparition (optionnelle : sinon InteractAction sert aussi à réapparaître). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> RespawnAction;

	/** Consommation rapide (optionnelle) : consomme QuickConsumableId. L'inventaire passe, lui, par RequestConsume. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> QuickConsumeAction;

	/** Consommable de la touche rapide (identifiant de DT_Consumables). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	FName QuickConsumableId;

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

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UEMBSurvivalComponent> Survival;

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

	// ---------------------------------------------------------------- Mort, réapparition, réanimation

	void OnRespawnPressed();
	void OnQuickConsumePressed();

	/** Serveur : passe en attente de réapparition (appelé sur l'entrée en état Dead). */
	void BeginDeath();

	/** Serveur : sort de l'attente (réanimation). La réapparition, elle, détruit ce pawn (AEMBGameMode::RespawnPlayer). */
	void EndDeathByRevive();

	/** Serveur et clients : coupe ou rétablit le déplacement selon l'état. */
	void ApplyDeadState(bool bDead);

	/** Serveur : demande la réapparition via le mode de jeu (échéance automatique ou demande du joueur). */
	void RespawnNow();

	/** Serveur : y a-t-il un autre joueur vivant (et non en attente) capable de réanimer ce personnage ? */
	bool HasLivingAlly() const;

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RequestRespawn();

	UFUNCTION()
	void OnRep_DeadAwaitingRespawn();

	/** Répliqué à tous : l'état de mort est visible des alliés (interaction de réanimation, UI de groupe). */
	UPROPERTY(ReplicatedUsing = OnRep_DeadAwaitingRespawn)
	bool bDeadAwaitingRespawn;

	/** Horloge serveur au moment de la mort (calcul du temps restant côté client). */
	UPROPERTY(Replicated)
	double DeathServerTime;

	FTimerHandle AutoRespawnTimerHandle;

	/** Serveur : cause de la mort (zone létale ou chute hors du monde). */
	bool bKilledInLethalZone = false;
	bool bFellOutOfWorld = false;

	// ---------------------------------------------------------------- Interaction

	void OnInteractStarted();
	void OnInteractCompleted();

	/** Client local : balaie les alentours (timer) et met à jour la cible et l'invite. */
	void UpdateInteractionTarget();
	void SetInteractionTarget(AActor* NewTarget, const FText& NewPrompt);

	/** Distance (cm) entre ce personnage et le point le plus proche de la cible ; centre de l'acteur à défaut de collision. */
	float DistanceToInteractable(const AActor* Target) const;

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_Interact(AActor* Target);

	UFUNCTION(Server, Reliable)
	void Server_CancelInteract();

	/** Informe le client propriétaire qu'une interaction maintenue s'est terminée (réussie ou interrompue). */
	UFUNCTION(Client, Reliable)
	void Client_InteractionEnded(bool bCompleted);

	/** Serveur : l'instigateur (ce personnage) peut-il agir : vivant, non étourdi, pas en attente de réapparition ? */
	bool CanActNow() const;
	void ServerTickInteractionHold();
	void ServerEndInteractionHold(bool bCompleted);

	TWeakObjectPtr<AActor> InteractionTarget;
	FText InteractionPrompt;
	FTimerHandle InteractionScanTimerHandle;

	// Maintien côté client (estimation pour l'interface)
	bool bLocalHolding;
	double LocalHoldStartTime;
	float LocalHoldDuration;

	// Maintien côté serveur
	TWeakObjectPtr<AActor> HoldTarget;
	double HoldStartTime;
	float HoldDuration;
	FTimerHandle InteractionHoldTimerHandle;

	// Anti-inondation des RPC d'interaction (serveur)
	double InteractWindowStart;
	int32 InteractRequestCount;

	FTimerHandle SprintDrainTimerHandle;
	double LastConfirmPlacementTime;
};
