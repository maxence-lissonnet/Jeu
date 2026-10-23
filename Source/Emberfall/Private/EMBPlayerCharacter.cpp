// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBPlayerCharacter.h"
#include "Emberfall.h"
#include "EMBBuildingComponent.h"
#include "EMBCharacterMovementComponent.h"
#include "EMBCombatComponent.h"
#include "EMBInventoryComponent.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBStatsComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "TimerManager.h"

namespace
{
	/** Période de facturation de l'endurance de sprint (serveur). 5 prélèvements/s : assez fin, très peu coûteux. */
	constexpr float SprintDrainInterval = 0.2f;

	/** Vitesse minimale (cm/s) pour considérer qu'on court réellement (pas de coût à l'arrêt). */
	constexpr float SprintMinSpeed = 50.f;

	/** Anti-rebond de la confirmation de pose : un même clic ne doit jamais confirmer deux fois. */
	constexpr double ConfirmPlacementDebounce = 0.15;
}

AEMBPlayerCharacter::AEMBPlayerCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UEMBCharacterMovementComponent>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = false; // Aucune logique par frame : tout est événementiel.

	MappingContextPriority = 0;
	LastConfirmPlacementTime = -1.0;

	// Le personnage s'oriente selon son déplacement, la caméra selon la souris.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = true;
	Movement->RotationRate = FRotator(0.f, 540.f, 0.f);
	Movement->BrakingDecelerationWalking = 2048.f;

	/*
	 * Serveur dédié : par défaut, un mesh non rendu n'évalue pas son animation (VisibilityBasedAnimTickOption).
	 * Or toute la détection de coups (sockets de l'arme), les fenêtres de combo/i-frames (notifies) et la
	 * fin des montages en dépendent. On force donc l'évaluation de la pose ET le rafraîchissement des os.
	 * Coût : une évaluation d'animation par personnage joueur (1 à 4 en coop), négligeable ; à ne PAS
	 * généraliser aux PNJ lointains.
	 */
	GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 380.f;
	SpringArm->SocketOffset = FVector(0.f, 0.f, 60.f);
	SpringArm->bUsePawnControlRotation = true;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 12.f;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	Stats = CreateDefaultSubobject<UEMBStatsComponent>(TEXT("Stats"));
	Combat = CreateDefaultSubobject<UEMBCombatComponent>(TEXT("Combat"));
	Inventory = CreateDefaultSubobject<UEMBInventoryComponent>(TEXT("Inventory"));
	Building = CreateDefaultSubobject<UEMBBuildingComponent>(TEXT("Building"));
}

void AEMBPlayerCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (Combat)
	{
		Combat->OnCombatStateChanged.AddUniqueDynamic(this, &AEMBPlayerCharacter::HandleCombatStateChanged);
	}

	// La charge portée détermine la roulade (légère / moyenne / lourde / interdite) : on la recalcule à chaque
	// changement d'inventaire. Fait sur toutes les machines (le client prédit la roulade avec la même valeur).
	if (Inventory)
	{
		Inventory->OnInventoryChanged.AddUniqueDynamic(this, &AEMBPlayerCharacter::HandleInventoryChanged);
		UpdateEncumbrance();
	}
}

void AEMBPlayerCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Le pawn est détruit à la mort puis recréé : la progression vit sur le PlayerState, elle repousse donc
	// ses bonus vers chaque nouveau pawn. Ensuite le composant les remet à jour tout seul à chaque changement.
	if (AEMBPlayerState* EMBState = GetPlayerState<AEMBPlayerState>())
	{
		if (UEMBProgressionComponent* Progression = EMBState->GetProgressionComponent())
		{
			Progression->ApplyModifiersToPawn(this);
		}
	}
	UpdateEncumbrance();
}

void AEMBPlayerCharacter::HandleInventoryChanged(FName ResourceId, int32 NewCount)
{
	UpdateEncumbrance();
}

void AEMBPlayerCharacter::UpdateEncumbrance()
{
	if (!Inventory || !Combat)
	{
		return;
	}
	const float MaxWeight = Inventory->GetMaxCarryWeight();
	Combat->SetEncumbranceRatio(MaxWeight > KINDA_SMALL_NUMBER ? Inventory->GetCurrentWeight() / MaxWeight : 0.f);
}

UEMBCharacterMovementComponent* AEMBPlayerCharacter::GetEMBMovement() const
{
	return Cast<UEMBCharacterMovementComponent>(GetCharacterMovement());
}

void AEMBPlayerCharacter::TryConfirmPlacement()
{
	if (!IsInBuildMode() || !GetWorld())
	{
		return;
	}

	// Une seule voie : attaque légère et touche "confirmer" passent ici. Si les deux touches sont liées au
	// même clic, le second appel tombe dans la fenêtre d'anti-rebond et est ignoré.
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - LastConfirmPlacementTime < ConfirmPlacementDebounce)
	{
		return;
	}
	LastConfirmPlacementTime = Now;
	Building->ConfirmPlacement();
}

void AEMBPlayerCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(SprintDrainTimerHandle);
	Super::EndPlay(EndPlayReason);
}

// ================================================================================================
// Entrées
// ================================================================================================

void AEMBPlayerCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();

	// Appelé sur le client qui contrôle ce pawn (et sur l'hôte d'une partie listen) à chaque (re)prise de contrôle :
	// c'est le bon endroit pour installer le contexte de mapping. Jamais appelé sur un serveur dédié.
	const APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController || !DefaultMappingContext)
	{
		return;
	}

	if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
	{
		InputSubsystem->AddMappingContext(DefaultMappingContext, MappingContextPriority);
	}
}

void AEMBPlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!Input)
	{
		UE_LOG(LogEmberfall, Error, TEXT("AEMBPlayerCharacter exige UEnhancedInputComponent (Project Settings > Input > Default Input Component Class)."));
		return;
	}

	// Une action non assignée dans le Blueprint est simplement ignorée (jamais de BindAction sur nullptr).
	if (MoveAction)
	{
		Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AEMBPlayerCharacter::OnMove);
	}
	if (LookAction)
	{
		Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &AEMBPlayerCharacter::OnLook);
	}
	if (JumpAction)
	{
		Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	}
	if (SprintAction)
	{
		Input->BindAction(SprintAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnSprintStarted);
		Input->BindAction(SprintAction, ETriggerEvent::Completed, this, &AEMBPlayerCharacter::OnSprintCompleted);
		Input->BindAction(SprintAction, ETriggerEvent::Canceled, this, &AEMBPlayerCharacter::OnSprintCompleted);
	}
	if (LightAttackAction)
	{
		Input->BindAction(LightAttackAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnLightAttackStarted);
	}
	if (HeavyAttackAction)
	{
		// Started = début de charge ; Completed/Canceled = relâchement (le composant décide lourde ou chargée).
		Input->BindAction(HeavyAttackAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnHeavyAttackStarted);
		Input->BindAction(HeavyAttackAction, ETriggerEvent::Completed, this, &AEMBPlayerCharacter::OnHeavyAttackReleased);
		Input->BindAction(HeavyAttackAction, ETriggerEvent::Canceled, this, &AEMBPlayerCharacter::OnHeavyAttackReleased);
	}
	if (DodgeAction)
	{
		Input->BindAction(DodgeAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnDodgeStarted);
	}
	if (ParryAction)
	{
		Input->BindAction(ParryAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnParryStarted);
	}
	if (BlockAction)
	{
		Input->BindAction(BlockAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnBlockStarted);
		Input->BindAction(BlockAction, ETriggerEvent::Completed, this, &AEMBPlayerCharacter::OnBlockReleased);
		Input->BindAction(BlockAction, ETriggerEvent::Canceled, this, &AEMBPlayerCharacter::OnBlockReleased);
	}
	if (ToggleBuildAction)
	{
		Input->BindAction(ToggleBuildAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnToggleBuild);
	}
	if (RotatePieceAction)
	{
		Input->BindAction(RotatePieceAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnRotatePiece);
	}
	if (ConfirmPlaceAction)
	{
		Input->BindAction(ConfirmPlaceAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnConfirmPlace);
	}
	if (CancelAction)
	{
		Input->BindAction(CancelAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnCancel);
	}
}

bool AEMBPlayerCharacter::IsInBuildMode() const
{
	return Building && Building->IsInBuildMode();
}

void AEMBPlayerCharacter::SendCombatInput(EEMBCombatInput Input)
{
	if (Combat && !IsInBuildMode())
	{
		Combat->HandleInput(Input);
	}
}

void AEMBPlayerCharacter::OnMove(const FInputActionValue& Value)
{
	if (!Controller || !Combat)
	{
		return;
	}

	// Étourdi ou mort : aucun contrôle du déplacement.
	const EEMBCombatState State = Combat->GetCurrentState();
	if (State == EEMBCombatState::Staggered || State == EEMBCombatState::Dead)
	{
		return;
	}

	const FVector2D Axis = Value.Get<FVector2D>();
	const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);
	const FVector Forward = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

	AddMovementInput(Forward, Axis.Y);
	AddMovementInput(Right, Axis.X);
}

void AEMBPlayerCharacter::OnLook(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(Axis.Y);
}

void AEMBPlayerCharacter::OnLightAttackStarted()
{
	// En mode construction, l'attaque légère confirme le placement de la pièce.
	if (IsInBuildMode())
	{
		TryConfirmPlacement();
		return;
	}
	SendCombatInput(EEMBCombatInput::LightAttack);
}

void AEMBPlayerCharacter::OnHeavyAttackStarted()
{
	SendCombatInput(EEMBCombatInput::ChargedAttackStart);
}

void AEMBPlayerCharacter::OnHeavyAttackReleased()
{
	// Le relâchement est toujours transmis (même en mode construction) pour ne jamais laisser une charge bloquée.
	if (Combat)
	{
		Combat->HandleInput(EEMBCombatInput::ChargedAttackRelease);
	}
}

void AEMBPlayerCharacter::OnDodgeStarted()
{
	SendCombatInput(EEMBCombatInput::Dodge);
}

void AEMBPlayerCharacter::OnParryStarted()
{
	SendCombatInput(EEMBCombatInput::Parry);
}

void AEMBPlayerCharacter::OnBlockStarted()
{
	SendCombatInput(EEMBCombatInput::BlockStart);
}

void AEMBPlayerCharacter::OnBlockReleased()
{
	if (Combat)
	{
		Combat->HandleInput(EEMBCombatInput::BlockEnd);
	}
}

void AEMBPlayerCharacter::OnToggleBuild()
{
	if (!Building || !Combat)
	{
		return;
	}
	// On n'entre en mode construction qu'au repos ; on peut toujours en sortir.
	if (!Building->IsInBuildMode() && Combat->GetCurrentState() != EEMBCombatState::Idle)
	{
		return;
	}
	Building->ToggleBuildMode();
}

void AEMBPlayerCharacter::OnRotatePiece(const FInputActionValue& Value)
{
	if (IsInBuildMode())
	{
		Building->RotatePreview(Value.Get<float>() >= 0.f ? 1.f : -1.f);
	}
}

void AEMBPlayerCharacter::OnConfirmPlace()
{
	TryConfirmPlacement();
}

void AEMBPlayerCharacter::OnCancel()
{
	if (IsInBuildMode())
	{
		Building->CancelBuildMode();
	}
}

// ================================================================================================
// État de combat
// ================================================================================================

void AEMBPlayerCharacter::HandleCombatStateChanged(EEMBCombatState OldState, EEMBCombatState NewState)
{
	// Exécuté sur le serveur (TransitionTo) et sur les clients (RepNotify de l'état).
	if (NewState == EEMBCombatState::Staggered || NewState == EEMBCombatState::Dead)
	{
		ForceStopSprint();
	}

	if (NewState == EEMBCombatState::Dead)
	{
		GetCharacterMovement()->DisableMovement();
		if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
		{
			DisableInput(PlayerController);
		}
	}
}

// ================================================================================================
// Saut (coût d'endurance 10)
// ================================================================================================

bool AEMBPlayerCharacter::CanJumpInternal_Implementation() const
{
	// Sauter exige de la jauge (règle souls : > 0) et un état de combat libre.
	return Super::CanJumpInternal_Implementation()
		&& Stats && Stats->CanAffordStamina()
		&& Combat && Combat->GetCurrentState() == EEMBCombatState::Idle;
}

void AEMBPlayerCharacter::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();

	// Le coût est facturé par le serveur uniquement (les jauges ne sont jamais modifiées par un client).
	if (HasAuthority() && Stats)
	{
		Stats->ConsumeStamina(EMBCombatDefaults::JumpStaminaCost);
	}
}

// ================================================================================================
// Sprint (8 endurance/s)
// ================================================================================================

bool AEMBPlayerCharacter::IsSprinting() const
{
	const UEMBCharacterMovementComponent* Movement = GetEMBMovement();
	return Movement && Movement->IsSprintActive();
}

void AEMBPlayerCharacter::OnSprintStarted()
{
	UEMBCharacterMovementComponent* Movement = GetEMBMovement();
	if (!Movement || !Stats || !Stats->CanAffordStamina() || IsInBuildMode())
	{
		return;
	}
	// Prédiction propriétaire : le souhait voyage ensuite dans les flags de chaque mouvement client.
	// Le serveur le reçoit (UpdateFromCompressedFlags) et appelle NotifySprintRequested.
	Movement->SetWantsToSprint(true);
}

void AEMBPlayerCharacter::OnSprintCompleted()
{
	if (UEMBCharacterMovementComponent* Movement = GetEMBMovement())
	{
		Movement->SetWantsToSprint(false);
	}
}

void AEMBPlayerCharacter::NotifySprintRequested(bool bRequested)
{
	if (!HasAuthority())
	{
		return;
	}

	if (!bRequested)
	{
		GetWorldTimerManager().ClearTimer(SprintDrainTimerHandle);
		return;
	}

	// Sprint demandé avec une jauge vide, ou dans un état qui l'interdit : refus immédiat.
	const EEMBCombatState State = Combat ? Combat->GetCurrentState() : EEMBCombatState::Idle;
	if (!Stats || !Stats->CanAffordStamina() || State == EEMBCombatState::Staggered || State == EEMBCombatState::Dead)
	{
		ForceStopSprint();
		return;
	}

	// Facturation continue côté serveur par timer (pas de Tick).
	GetWorldTimerManager().SetTimer(SprintDrainTimerHandle, this, &AEMBPlayerCharacter::DrainSprintStamina, SprintDrainInterval, true);
}

void AEMBPlayerCharacter::ForceStopSprint()
{
	UEMBCharacterMovementComponent* Movement = GetEMBMovement();
	if (!Movement)
	{
		return;
	}

	if (HasAuthority())
	{
		GetWorldTimerManager().ClearTimer(SprintDrainTimerHandle);
		if (!IsLocallyControlled())
		{
			// Joueur distant : le serveur ignore son souhait de sprint jusqu'au relâchement de la touche
			// (on ne touche pas au souhait reçu, sinon chaque mouvement client le rétablirait et
			// relancerait la procédure), et le prévient pour qu'il coupe aussi sa prédiction.
			Movement->ForbidSprint();
			Client_StopSprint();
			return;
		}
	}
	// Propriétaire local (client, ou hôte d'une partie listen) : on coupe le souhait directement.
	Movement->SetWantsToSprint(false);
}

void AEMBPlayerCharacter::Client_StopSprint_Implementation()
{
	if (UEMBCharacterMovementComponent* Movement = GetEMBMovement())
	{
		Movement->SetWantsToSprint(false);
	}
}

void AEMBPlayerCharacter::DrainSprintStamina()
{
	const UEMBCharacterMovementComponent* Movement = GetEMBMovement();
	if (!Movement || !Movement->WantsToSprint() || !Stats)
	{
		GetWorldTimerManager().ClearTimer(SprintDrainTimerHandle);
		return;
	}

	// Le timer reste armé, mais on ne facture que si le sprint est réellement actif (autorisé, au sol).
	if (!Movement->IsSprintActive())
	{
		return;
	}

	// À l'arrêt, le sprint ne coûte rien (et ne bloque pas la régénération).
	if (GetVelocity().SizeSquared2D() < FMath::Square(SprintMinSpeed))
	{
		return;
	}

	Stats->ConsumeStamina(EMBCombatDefaults::SprintStaminaPerSecond * SprintDrainInterval);
	if (!Stats->CanAffordStamina())
	{
		ForceStopSprint(); // Endurance à 0 : arrêt forcé, serveur et client propriétaire.
	}
}
