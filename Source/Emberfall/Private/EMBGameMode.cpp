// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBGameMode.h"
#include "EMBPlayerCharacter.h"
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
