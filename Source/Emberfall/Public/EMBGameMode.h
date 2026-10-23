// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "EMBGameMode.generated.h"

/**
 * Mode de jeu d'Emberfall (serveur). Branche la sauvegarde (UEMBSaveSubsystem) sur le cycle de vie de la partie :
 *  - InitGame : choix du slot via l'option d'URL ?Slot=N (0 par défaut) ;
 *  - StartPlay : chargement du monde ;
 *  - PostLogin : restauration du joueur qui se connecte ;
 *  - Logout : capture du joueur qui part.
 *  - EndPlay (arrêt du serveur ou fin de session éditeur) : dernière sauvegarde bloquante.
 */
UCLASS()
class EMBERFALL_API AEMBGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AEMBGameMode();

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void StartPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void HandleSeamlessTravelPlayer(AController*& C) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Logout(AController* Exiting) override;
};
