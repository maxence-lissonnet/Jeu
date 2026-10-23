// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "EMBDiscoveryVolume.generated.h"

class UBoxComponent;

/**
 * Volume de découverte : le premier passage d'un joueur donne l'événement (Event.Location.Discovered.*),
 * de l'XP et éventuellement un savoir. Valable pour ce joueur seulement (une découverte n'est pas partagée).
 * Détection côté serveur uniquement ; chaque joueur ne profite de la récompense qu'une fois.
 */
UCLASS()
class EMBERFALL_API AEMBDiscoveryVolume : public AActor
{
	GENERATED_BODY()

public:
	AEMBDiscoveryVolume();

	/** Événement accordé (ex. EMBTags::Event_Location_Discovered_StarWell). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Discovery")
	FGameplayTag DiscoveryTag;

	/** Savoir (lore) accordé en plus, optionnel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Discovery")
	FName GrantedKnowledge;

	/** XP accordée à la première découverte. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Discovery", meta = (ClampMin = "0"))
	int64 XPReward;

protected:
	virtual void BeginPlay() override;

private:
	UFUNCTION()
	void OnVolumeBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UBoxComponent> Box;
};
