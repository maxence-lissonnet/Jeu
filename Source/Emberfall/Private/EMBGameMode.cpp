// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBGameMode.h"
#include "Emberfall.h"
#include "EMBCombatComponent.h"
#include "EMBCombatTypes.h"
#include "EMBPlayerCharacter.h"
#include "EMBRespawnSubsystem.h"
#include "EMBSurvivalComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "EMBPlayerState.h"
#include "EMBSaveSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

AEMBGameMode::AEMBGameMode()
{
	PlayerStateClass = AEMBPlayerState::StaticClass();
	DefaultPawnClass = AEMBPlayerCharacter::StaticClass();
}

void AEMBGameMode::RespawnPlayer(AController* Controller)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !World || !IsValid(Controller))
	{
		return;
	}

	APawn* OldPawn = Controller->GetPawn();

	// Garde-fou : on ne "réapparaît" jamais un joueur vivant (appel erroné, doublon de requête, timer périmé).
	// Sans pawn (détruit, déconnexion partielle), la réapparition reste permise.
	const AEMBPlayerCharacter* DeadCharacter = Cast<AEMBPlayerCharacter>(OldPawn);
	if (DeadCharacter && !DeadCharacter->IsDeadAwaitingRespawn())
	{
		return;
	}

	FVector BagLocation = OldPawn ? OldPawn->GetActorLocation() : FVector::ZeroVector;

	// Mort en zone létale (coup fatal marqué bLethalZone : lave, vide ; ou chute hors du monde) : le corps est souvent inaccessible (lave,
	// vide). Le sac est alors déposé au dernier point sûr connu du joueur, relevé par la sauvegarde. Si la
	// sauvegarde n'a aucun point sûr pour lui, on garde le lieu de la mort (cas rare : mort dans les 10 s
	// qui suivent la connexion).
	if (DeadCharacter && DeadCharacter->WasKilledInLethalZone())
	{
		if (const UGameInstance* GameInstance = GetGameInstance())
		{
			if (const UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
			{
				FTransform SafeTransform;
				if (Save->GetLastSafeTransform(Save->GetPlayerKey(Cast<APlayerController>(Controller)), SafeTransform))
				{
					BagLocation = SafeTransform.GetLocation();
				}
			}
		}
	}

	// 1) Sac de mort : AVANT de détruire le pawn, car il lit l'inventaire du pawn mort.
	UEMBRespawnSubsystem* RespawnSystem = World->GetSubsystem<UEMBRespawnSubsystem>();
	if (RespawnSystem && OldPawn)
	{
		RespawnSystem->DropDeathBag(Controller, BagLocation);
	}

	// 2) Point de réapparition : point lié valide (lit, feu de camp), sinon PlayerStart.
	FTransform SpawnTransform = FTransform::Identity;
	const bool bHasLinkedPoint = RespawnSystem && RespawnSystem->GetRespawnTransform(Controller, SpawnTransform);
	if (!bHasLinkedPoint)
	{
		if (const AActor* PlayerStart = FindPlayerStart(Controller))
		{
			// Sans échelle : l'échelle d'un PlayerStart (souvent 1, parfois modifiée) ne doit pas déformer le pawn.
			SpawnTransform = FTransform(PlayerStart->GetActorRotation(), PlayerStart->GetActorLocation());
		}
		else
		{
			SpawnTransform.SetLocation(BagLocation); // Aucun PlayerStart : mieux vaut réapparaître sur place que pas du tout.
			UE_LOG(LogEmberfall, Warning, TEXT("RespawnPlayer : aucun PlayerStart dans le niveau, réapparition sur le lieu de la mort."));
		}
	}

	// 3) Ancien pawn détruit, nouveau pawn créé. Le détacher d'abord évite que le contrôleur garde une référence
	// morte et que RestartPlayerAtTransform ne remplace un pawn encore vivant dans le monde.
	if (OldPawn)
	{
		Controller->UnPossess();
		OldPawn->Destroy();
	}
	SpawnTransform.SetScale3D(FVector::OneVector); // Jamais d'échelle héritée du point lié ou du PlayerStart.
	RestartPlayerAtTransform(Controller, SpawnTransform);

	// 4) Valeurs de réapparition sur le nouveau pawn (PossessedBy a déjà appliqué la progression).
	if (APawn* NewPawn = Controller->GetPawn())
	{
		if (UEMBSurvivalComponent* Survival = NewPawn->FindComponentByClass<UEMBSurvivalComponent>())
		{
			Survival->ApplyRespawnValues(); // Faim 60, soif 60, Corruption 0, température neutre.
		}
		if (UEMBCombatComponent* Combat = NewPawn->FindComponentByClass<UEMBCombatComponent>())
		{
			Combat->GrantSpawnProtection(EMBCombatDefaults::SpawnProtectionDuration);
		}
	}
}

void AEMBGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			// Lie la sauvegarde au monde de cette partie AVANT de choisir le slot.
			Save->BindToWorld(GetWorld());
			Save->SetActiveSlot(UGameplayStatics::GetIntOption(Options, TEXT("Slot"), 0));
		}
	}
}

void AEMBGameMode::StartPlay()
{
	Super::StartPlay();

	// Après Super : le monde et ses acteurs existent, on peut y restaurer constructions et événements.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			Save->LoadActiveSlot();
		}
	}
}

void AEMBGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			Save->RestorePlayer(NewPlayer);
		}
	}
}

void AEMBGameMode::HandleSeamlessTravelPlayer(AController*& C)
{
	Super::HandleSeamlessTravelPlayer(C);

	// Après un voyage transparent PostLogin n'est pas rappelé : on restaure le joueur ici.
	if (APlayerController* PlayerController = Cast<APlayerController>(C))
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
			{
				Save->RestorePlayer(PlayerController);
			}
		}
	}
}

void AEMBGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Arrêt volontaire du serveur / fin de session éditeur : dernière sauvegarde, bloquante (le processus
	// va s'arrêter, une écriture asynchrone n'aurait pas le temps de finir). Pas lors d'un simple changement
	// de niveau ou d'une destruction : l'autosave couvre ces cas.
	if (HasAuthority() && (EndPlayReason == EEndPlayReason::Quit || EndPlayReason == EEndPlayReason::EndPlayInEditor
		|| EndPlayReason == EEndPlayReason::LevelTransition))
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
			{
				Save->SaveNowBlocking();
			}
		}
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBGameMode::Logout(AController* Exiting)
{
	// Avant Super : le pawn et le PlayerState du joueur existent encore, on peut les capturer.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			Save->CapturePlayer(Cast<APlayerController>(Exiting));
		}
	}

	Super::Logout(Exiting);
}
