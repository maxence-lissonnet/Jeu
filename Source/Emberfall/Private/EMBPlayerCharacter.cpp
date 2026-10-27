// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBPlayerCharacter.h"
#include "Emberfall.h"
#include "EMBBuildingComponent.h"
#include "EMBCharacterMovementComponent.h"
#include "EMBCombatComponent.h"
#include "EMBGameMode.h"
#include "EMBGameplayTags.h"
#include "EMBInventoryComponent.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBStatsComponent.h"
#include "EMBSurvivalComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
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

	bRespawnRequiresNoLivingAlly = false; // Arbitrage du chef : tout joueur mort peut réapparaître dès 3 s.
	bDeadAwaitingRespawn = false;
	DeathServerTime = 0.0;
	bLocalHolding = false;
	LocalHoldStartTime = 0.0;
	LocalHoldDuration = 0.f;
	HoldStartTime = 0.0;
	HoldDuration = 0.f;
	InteractWindowStart = 0.0;
	InteractRequestCount = 0;

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
	Survival = CreateDefaultSubobject<UEMBSurvivalComponent>(TEXT("Survival"));
	QuickConsumableId = TEXT("HealingFlask");
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
	FTimerManager& TimerManager = GetWorldTimerManager();
	TimerManager.ClearTimer(SprintDrainTimerHandle);
	TimerManager.ClearTimer(AutoRespawnTimerHandle);
	TimerManager.ClearTimer(InteractionScanTimerHandle);
	TimerManager.ClearTimer(InteractionHoldTimerHandle);
	Super::EndPlay(EndPlayReason);
}

// ================================================================================================
// Entrées
// ================================================================================================

void AEMBPlayerCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();

	// Recherche de la cible d'interaction : uniquement pour le joueur local, 7 fois par seconde (pas de Tick).
	// Démarrée ici (et non au BeginPlay) car le contrôle local n'est connu qu'à la prise de contrôle.
	if (IsLocallyControlled())
	{
		GetWorldTimerManager().SetTimer(InteractionScanTimerHandle, this, &AEMBPlayerCharacter::UpdateInteractionTarget, 0.15f, true);
	}

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
	if (InteractAction)
	{
		// Started = début (ou interaction instantanée) ; Completed/Canceled = relâchement d'un maintien.
		Input->BindAction(InteractAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnInteractStarted);
		Input->BindAction(InteractAction, ETriggerEvent::Completed, this, &AEMBPlayerCharacter::OnInteractCompleted);
		Input->BindAction(InteractAction, ETriggerEvent::Canceled, this, &AEMBPlayerCharacter::OnInteractCompleted);
	}
	if (RespawnAction)
	{
		Input->BindAction(RespawnAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnRespawnPressed);
	}
	if (QuickConsumeAction)
	{
		Input->BindAction(QuickConsumeAction, ETriggerEvent::Started, this, &AEMBPlayerCharacter::OnQuickConsumePressed);
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
	if (State == EEMBCombatState::Staggered || State == EEMBCombatState::Dead || bDeadAwaitingRespawn)
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
		// Immédiat sur toutes les machines. On ne coupe PAS les entrées du contrôleur (DisableInput) : le joueur
		// mort doit encore pouvoir demander sa réapparition. Les actions de jeu sont refusées par le combat
		// (état Dead) et par les gardes des callbacks ci-dessous.
		GetCharacterMovement()->DisableMovement();

		if (HasAuthority())
		{
			BeginDeath();
		}
	}
}

// ================================================================================================
// Mort, réapparition, réanimation
// ================================================================================================

void AEMBPlayerCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AEMBPlayerCharacter, bDeadAwaitingRespawn);
	DOREPLIFETIME(AEMBPlayerCharacter, DeathServerTime);
}

void AEMBPlayerCharacter::GetOwnedGameplayTags(FGameplayTagContainer& TagContainer) const
{
	if (bDeadAwaitingRespawn)
	{
		TagContainer.AddTag(EMBTags::State_Player_DeadAwaitingRespawn.GetTag());
	}
	if (Survival && Survival->HasWhispers())
	{
		TagContainer.AddTag(EMBTags::State_Survival_Whispers.GetTag()); // Corruption >= 25.
	}
	if (Combat)
	{
		// Tags d'état du combat, dérivés de l'état courant (pas de duplication de données).
		if (Combat->HasStateTag(EMBTags::State_Combat_Invulnerable.GetTag()))
		{
			TagContainer.AddTag(EMBTags::State_Combat_Invulnerable.GetTag());
		}
		if (Combat->HasStateTag(EMBTags::State_Combat_HyperArmor.GetTag()))
		{
			TagContainer.AddTag(EMBTags::State_Combat_HyperArmor.GetTag());
		}
		if (Combat->HasStateTag(EMBTags::State_Combat_Staggered.GetTag()))
		{
			TagContainer.AddTag(EMBTags::State_Combat_Staggered.GetTag());
		}
		if (Combat->HasStateTag(EMBTags::State_Dead.GetTag()))
		{
			TagContainer.AddTag(EMBTags::State_Dead.GetTag());
		}
	}
}

void AEMBPlayerCharacter::BeginDeath()
{
	if (!HasAuthority() || bDeadAwaitingRespawn || !GetWorld())
	{
		return;
	}

	ServerEndInteractionHold(false); // Un mort n'interagit plus.

	// Zone létale = coup fatal marqué bLethalZone (lave, vide) ou chute hors du monde : le corps, donc le sac,
	// y serait inaccessible. Faim, froid, brûlure, intoxication : le sac reste sur place. Lu par RespawnPlayer.
	bKilledInLethalZone = bFellOutOfWorld || (Combat && Combat->WasKilledInLethalZone());

	bDeadAwaitingRespawn = true;
	DeathServerTime = GetWorld()->GetTimeSeconds();
	ApplyDeadState(true);

	// Réapparition automatique à 30 s : c'est la fenêtre pendant laquelle un allié peut réanimer.
	GetWorldTimerManager().SetTimer(AutoRespawnTimerHandle, this, &AEMBPlayerCharacter::RespawnNow, EMBCombatDefaults::RespawnAutoDelay, false);

	OnDeadAwaitingRespawnChanged.Broadcast(true);
	ForceNetUpdate();
}

void AEMBPlayerCharacter::EndDeathByRevive()
{
	if (!HasAuthority() || !bDeadAwaitingRespawn)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(AutoRespawnTimerHandle);
	bDeadAwaitingRespawn = false;
	ApplyDeadState(false);
	OnDeadAwaitingRespawnChanged.Broadcast(false);
	ForceNetUpdate();
}

void AEMBPlayerCharacter::ApplyDeadState(bool bDead)
{
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (bDead)
	{
		ForceStopSprint();
		Movement->DisableMovement();
		SetInteractionTarget(nullptr, FText::GetEmpty());
	}
	else
	{
		Movement->SetMovementMode(MOVE_Walking);
	}
}

void AEMBPlayerCharacter::OnRep_DeadAwaitingRespawn()
{
	ApplyDeadState(bDeadAwaitingRespawn);
	OnDeadAwaitingRespawnChanged.Broadcast(bDeadAwaitingRespawn);
}

void AEMBPlayerCharacter::FellOutOfWorld(const UDamageType& DamageType)
{
	// Le comportement de base détruit le pawn sur-le-champ : plus de sac, plus de réapparition propre.
	// On tue donc par le flux normal (OnDeath -> état Dead -> attente de réapparition). Le mouvement coupé
	// (ApplyDeadState) arrête la chute ; le moteur rappelle cette fonction à chaque tick sous la KillZ, d'où la garde.
	if (!HasAuthority() || !Stats)
	{
		Super::FellOutOfWorld(DamageType);
		return;
	}
	if (bFellOutOfWorld || bDeadAwaitingRespawn || Stats->IsDead())
	{
		return;
	}

	bFellOutOfWorld = true;
	Stats->ApplyDamage(Stats->GetHealth() + 1.f, nullptr);
}

float AEMBPlayerCharacter::GetAutoRespawnTimeRemaining() const
{
	const UWorld* World = GetWorld();
	if (!bDeadAwaitingRespawn || !World)
	{
		return 0.f;
	}
	// Horloge serveur estimée côté client (GameState) ; sur le serveur, l'horloge monde.
	const AGameStateBase* GameState = World->GetGameState();
	const double Now = GameState ? GameState->GetServerWorldTimeSeconds() : World->GetTimeSeconds();
	return static_cast<float>(FMath::Max(0.0, DeathServerTime + EMBCombatDefaults::RespawnAutoDelay - Now));
}

void AEMBPlayerCharacter::RespawnNow()
{
	if (!HasAuthority() || !bDeadAwaitingRespawn)
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(AutoRespawnTimerHandle);

	// Le mode de jeu dépose le sac, choisit le point, détruit CE pawn et en crée un nouveau.
	if (AEMBGameMode* GameMode = GetWorld()->GetAuthGameMode<AEMBGameMode>())
	{
		if (AController* OwnerController = GetController())
		{
			GameMode->RespawnPlayer(OwnerController);
		}
	}
}

bool AEMBPlayerCharacter::HasLivingAlly() const
{
	const UWorld* World = GetWorld();
	const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	if (!GameState)
	{
		return false;
	}
	for (const APlayerState* Other : GameState->PlayerArray)
	{
		const AEMBPlayerCharacter* OtherCharacter = Other ? Cast<AEMBPlayerCharacter>(Other->GetPawn()) : nullptr;
		if (OtherCharacter && OtherCharacter != this && !OtherCharacter->bDeadAwaitingRespawn)
		{
			return true;
		}
	}
	return false;
}

void AEMBPlayerCharacter::RequestRespawn()
{
	// Client -> serveur. Sur un hôte, la RPC s'exécute localement.
	if (bDeadAwaitingRespawn)
	{
		Server_RequestRespawn();
	}
}

bool AEMBPlayerCharacter::Server_RequestRespawn_Validate()
{
	return true;
}

void AEMBPlayerCharacter::Server_RequestRespawn_Implementation()
{
	if (!bDeadAwaitingRespawn || !GetWorld())
	{
		return;
	}

	const double Elapsed = GetWorld()->GetTimeSeconds() - DeathServerTime;
	if (Elapsed < EMBCombatDefaults::RespawnMinDelay)
	{
		return; // Trop tôt : évite une réapparition par appui réflexe, et laisse le temps de voir la mort.
	}

	// Solo ou sans allié vivant (personne ne peut réanimer) : réapparition possible dès 3 s. Avec un allié
	// vivant, on attend l'échéance automatique de 30 s pour lui laisser le temps de réanimer.
	if (bRespawnRequiresNoLivingAlly && HasLivingAlly() && Elapsed < EMBCombatDefaults::RespawnAutoDelay)
	{
		return;
	}
	RespawnNow();
}

void AEMBPlayerCharacter::OnRespawnPressed()
{
	RequestRespawn();
}

void AEMBPlayerCharacter::OnQuickConsumePressed()
{
	// Le serveur valide tout (stock, délai de 1 s, plafond de requêtes) ; ici on ne filtre que l'évident.
	if (Survival && !QuickConsumableId.IsNone() && !bDeadAwaitingRespawn)
	{
		Survival->RequestConsume(QuickConsumableId);
	}
}

// ================================================================================================
// Réanimation (IEMBInteractable)
// ================================================================================================

bool AEMBPlayerCharacter::CanInteract(const APawn* InInstigator) const
{
	// Serveur et client (invite) : un joueur mort en attente, réanimable par un autre joueur vivant.
	// Le PlayerState sert de critère "contrôlé par un joueur" : IsPlayerControlled() dépend du contrôleur,
	// qui n'existe pas chez les clients distants.
	const AEMBPlayerCharacter* Rescuer = Cast<AEMBPlayerCharacter>(InInstigator);
	return bDeadAwaitingRespawn
		&& Rescuer && Rescuer != this
		&& !Rescuer->bDeadAwaitingRespawn
		&& Rescuer->GetPlayerState() != nullptr && GetPlayerState() != nullptr;
}

void AEMBPlayerCharacter::Interact(APawn* InInstigator)
{
	// Serveur uniquement. Revérifie : l'état peut avoir changé pendant les 4 s de maintien.
	if (!HasAuthority() || !CanInteract(InInstigator) || !Stats || !Combat)
	{
		return;
	}

	// Réanimé : pas de sac (il n'est déposé qu'à la réapparition), inventaire intact.
	Stats->ReviveWithFraction(EMBCombatDefaults::ReviveHealthFraction, EMBCombatDefaults::ReviveStaminaFraction);
	if (Survival)
	{
		Survival->ApplyReviveValues(); // Mort par Corruption : plafonnée à 60 ; intoxication purgée (sinon mort immédiate).
	}
	bKilledInLethalZone = false;
	bFellOutOfWorld = false;
	Combat->ResetFromDeath();
	Combat->GrantSpawnProtection(EMBCombatDefaults::SpawnProtectionDuration);
	EndDeathByRevive();
}

FText AEMBPlayerCharacter::GetInteractionText(const APawn* InInstigator) const
{
	return NSLOCTEXT("EMBPlayer", "Revive", "Réanimer");
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
	if (!Movement || !Stats || !Stats->CanAffordStamina() || IsInBuildMode() || bDeadAwaitingRespawn)
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

// ================================================================================================
// Interaction
// ================================================================================================

namespace
{
	/** Marge (cm) ajoutée à la portée côté serveur : le joueur a pu bouger entre la requête et son arrivée. */
	constexpr float InteractServerTolerance = 50.f;

	/** Plafond de requêtes d'interaction par seconde et par joueur (au-delà : ignorées, sans déconnexion). */
	constexpr int32 MaxInteractRequestsPerSecond = 10;

	/** Un maintien se termine un peu avant l'échéance exacte : le contrôle serveur ne tombe qu'aux multiples de 0,25 s. */
	constexpr double HoldCompletionSlack = 0.05;
}

float AEMBPlayerCharacter::DistanceToInteractable(const AActor* Target) const
{
	if (!Target)
	{
		return TNumericLimits<float>::Max();
	}

	// Point de collision le plus proche : une grande pièce (lit, établi) est "près" dès que son bord l'est,
	// pas seulement son centre. Sans collision exploitable, on retombe sur la distance au centre.
	FVector Closest;
	const float Distance = Target->ActorGetDistanceToCollision(GetActorLocation(), ECC_Pawn, Closest);
	return Distance >= 0.f ? Distance : FVector::Dist(GetActorLocation(), Target->GetActorLocation());
}

bool AEMBPlayerCharacter::CanActNow() const
{
	if (!Combat || !Stats || bDeadAwaitingRespawn || Stats->IsDead())
	{
		return false;
	}
	const EEMBCombatState State = Combat->GetCurrentState();
	return State != EEMBCombatState::Dead && State != EEMBCombatState::Staggered;
}

void AEMBPlayerCharacter::UpdateInteractionTarget()
{
	AActor* Best = nullptr;
	FText BestPrompt;

	const UWorld* World = GetWorld();
	if (World && CanActNow() && !IsInBuildMode())
	{
		/*
		 * COÛT : un recouvrement sphérique de 250 cm, 7 fois par seconde, uniquement pour le joueur local.
		 * Il renvoie les composants de tous types d'objets proches ; on ne garde que les acteurs qui
		 * implémentent IEMBInteractable (test de cast, très bon marché). Le serveur n'exécute jamais ce balayage.
		 */
		FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBInteractScan), /*bTraceComplex=*/false, this);
		TArray<FOverlapResult> Overlaps;
		// Types utiles seulement (joueurs mort, sacs, pièces construites) : on évite physique, destructibles, etc.
		FCollisionObjectQueryParams ObjectTypes;
		ObjectTypes.AddObjectTypesToQuery(ECC_Pawn);
		ObjectTypes.AddObjectTypesToQuery(ECC_WorldDynamic);
		ObjectTypes.AddObjectTypesToQuery(ECC_WorldStatic);
		World->OverlapMultiByObjectType(
			Overlaps, GetActorLocation(), FQuat::Identity, ObjectTypes,
			FCollisionShape::MakeSphere(EMBCombatDefaults::InteractRange), Params);

		const FVector ViewOrigin = Camera ? Camera->GetComponentLocation() : GetActorLocation();
		const FVector ViewDirection = Camera ? Camera->GetForwardVector() : GetActorForwardVector();

		TSet<const AActor*> Seen;
		float BestScore = -TNumericLimits<float>::Max();
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* Candidate = Overlap.GetActor();
			if (!Candidate || Seen.Contains(Candidate))
			{
				continue;
			}
			Seen.Add(Candidate);

			const IEMBInteractable* Interactable = Cast<IEMBInteractable>(Candidate);
			if (!Interactable || !Interactable->CanInteract(this))
			{
				continue;
			}

			// Préférence à ce qui est face à la caméra, à égalité à ce qui est le plus proche. Rien n'est exclu :
			// un objet dans le dos reste utilisable s'il est seul.
			const float Facing = FVector::DotProduct(ViewDirection, (Candidate->GetActorLocation() - ViewOrigin).GetSafeNormal());
			const float Closeness = DistanceToInteractable(Candidate) / EMBCombatDefaults::InteractRange;
			const float Score = Facing - Closeness * 0.25f;
			if (Score > BestScore)
			{
				BestScore = Score;
				Best = Candidate;
				BestPrompt = Interactable->GetInteractionText(this);
			}
		}
	}

	SetInteractionTarget(Best, BestPrompt);
}

void AEMBPlayerCharacter::SetInteractionTarget(AActor* NewTarget, const FText& NewPrompt)
{
	if (InteractionTarget.Get() == NewTarget && InteractionPrompt.EqualTo(NewPrompt))
	{
		return;
	}
	InteractionTarget = NewTarget;
	InteractionPrompt = NewPrompt;
	OnInteractionTargetChanged.Broadcast(NewTarget, NewPrompt);
}

float AEMBPlayerCharacter::GetInteractionHoldProgress() const
{
	const UWorld* World = GetWorld();
	if (!bLocalHolding || !World || LocalHoldDuration <= KINDA_SMALL_NUMBER)
	{
		return 0.f;
	}
	return FMath::Clamp(static_cast<float>(World->GetTimeSeconds() - LocalHoldStartTime) / LocalHoldDuration, 0.f, 1.f);
}

void AEMBPlayerCharacter::OnInteractStarted()
{
	// Mort : la même touche demande la réapparition (refusée par le serveur avant 3 s).
	if (bDeadAwaitingRespawn)
	{
		RequestRespawn();
		return;
	}

	AActor* Target = InteractionTarget.Get();
	const IEMBInteractable* Interactable = Cast<IEMBInteractable>(Target);
	if (!Interactable || !GetWorld())
	{
		return;
	}

	// Maintien : on garde une estimation locale pour la barre de progression ; le serveur fait foi.
	const float HoldTime = Interactable->GetInteractionHoldTime();
	if (HoldTime > 0.f)
	{
		bLocalHolding = true;
		LocalHoldStartTime = GetWorld()->GetTimeSeconds();
		LocalHoldDuration = HoldTime;
	}
	Server_Interact(Target);
}

void AEMBPlayerCharacter::OnInteractCompleted()
{
	// Relâchement : on ne prévient le serveur que si un maintien était en cours (pas de RPC inutile).
	if (bLocalHolding)
	{
		bLocalHolding = false;
		Server_CancelInteract();
		OnInteractionHoldEnded.Broadcast(false);
	}
}

bool AEMBPlayerCharacter::Server_Interact_Validate(AActor* Target)
{
	return true; // Le serveur revérifie tout dans l'implémentation ; une cible périmée n'est pas une triche.
}

void AEMBPlayerCharacter::Server_Interact_Implementation(AActor* Target)
{
	if (!GetWorld())
	{
		return;
	}

	// Anti-inondation : au-delà de 10 requêtes par seconde, on ignore sans punir.
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now - InteractWindowStart >= 1.0)
	{
		InteractWindowStart = Now;
		InteractRequestCount = 0;
	}
	if (++InteractRequestCount > MaxInteractRequestsPerSecond)
	{
		return;
	}

	// Une nouvelle demande remplace un maintien précédent (sans prévenir le client : il vient de le relancer).
	GetWorldTimerManager().ClearTimer(InteractionHoldTimerHandle);
	HoldTarget.Reset();

	IEMBInteractable* Interactable = IsValid(Target) ? Cast<IEMBInteractable>(Target) : nullptr;
	const bool bValid = Interactable
		&& CanActNow()
		&& DistanceToInteractable(Target) <= EMBCombatDefaults::InteractRange + InteractServerTolerance
		&& Interactable->CanInteract(this);
	if (!bValid)
	{
		Client_InteractionEnded(false);
		return;
	}

	const float HoldTime = Interactable->GetInteractionHoldTime();
	if (HoldTime <= 0.f)
	{
		Interactable->Interact(this); // Interaction instantanée.
		return;
	}

	// Maintien : le serveur chronomètre et surveille (portée, vivant, non étourdi, cible toujours valide)
	// toutes les 0,25 s. Le client ne décide jamais de la fin.
	HoldTarget = Target;
	HoldStartTime = Now;
	HoldDuration = HoldTime;
	GetWorldTimerManager().SetTimer(InteractionHoldTimerHandle, this, &AEMBPlayerCharacter::ServerTickInteractionHold,
		EMBCombatDefaults::InteractHoldCheckInterval, true);
}

void AEMBPlayerCharacter::Server_CancelInteract_Implementation()
{
	ServerEndInteractionHold(false);
}

void AEMBPlayerCharacter::ServerTickInteractionHold()
{
	AActor* Target = HoldTarget.Get();
	IEMBInteractable* Interactable = IsValid(Target) ? Cast<IEMBInteractable>(Target) : nullptr;
	if (!Interactable
		|| !CanActNow()
		|| DistanceToInteractable(Target) > EMBCombatDefaults::InteractRange + InteractServerTolerance
		|| !Interactable->CanInteract(this))
	{
		ServerEndInteractionHold(false); // S'éloigne, meurt, est étourdi ou la cible n'est plus valide.
		return;
	}

	if (GetWorld()->GetTimeSeconds() - HoldStartTime >= HoldDuration - HoldCompletionSlack)
	{
		// Pendant le maintien la tolérance est large (50 cm) ; à l'instant de la réussite, la portée est stricte.
		if (DistanceToInteractable(Target) > EMBCombatDefaults::InteractRange)
		{
			ServerEndInteractionHold(false);
			return;
		}
		Interactable->Interact(this);
		ServerEndInteractionHold(true);
	}
}

void AEMBPlayerCharacter::ServerEndInteractionHold(bool bCompleted)
{
	GetWorldTimerManager().ClearTimer(InteractionHoldTimerHandle);
	if (HoldTarget.IsValid())
	{
		HoldTarget.Reset();
		Client_InteractionEnded(bCompleted); // Remet à zéro la barre de progression du joueur.
	}
}

void AEMBPlayerCharacter::Client_InteractionEnded_Implementation(bool bCompleted)
{
	if (bLocalHolding)
	{
		bLocalHolding = false;
		OnInteractionHoldEnded.Broadcast(bCompleted);
	}
}
