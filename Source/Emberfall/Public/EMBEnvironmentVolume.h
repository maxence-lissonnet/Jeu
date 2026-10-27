// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBSurvivalTypes.h"
#include "EMBWeatherTypes.h"
#include "EMBEnvironmentVolume.generated.h"

class UBoxComponent;

/**
 * Volume de biome : fixe la température de base et le taux de Corruption de la zone qu'il couvre.
 *
 * L'environnement (UEMBEnvironmentSubsystem) retient, pour un point donné, le volume de plus haute
 * Priority qui le contient ; hors de tout volume : 15 °C et 0 Corruption.
 * Un volume est purement géométrique (aucune collision, aucun événement de chevauchement) : il n'est
 * interrogé que par calcul, ce qui le rend gratuit en physique. Posé dans le niveau, il existe sur toutes
 * les machines et s'enregistre localement au BeginPlay.
 */
UCLASS()
class EMBERFALL_API AEMBEnvironmentVolume : public AActor
{
	GENERATED_BODY()

public:
	AEMBEnvironmentVolume();

	/** Température ambiante de base de la zone, en °C (avant nuit et météo). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environment", meta = (ClampMin = "-100", ClampMax = "100"))
	float BaseTemperatureC;

	/** Corruption ajoutée par seconde à un joueur dans la zone (0,5 pour la brume corrompue) ; ×2 la nuit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environment", meta = (ClampMin = "0"))
	float CorruptionRatePerSecond;

	/** Identifiant du biome : sa météo vient de la ligne du même nom dans DT_Weather (défaut « Temperate »). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environment")
	FName BiomeId;

	/** Plus la valeur est haute, plus le volume l'emporte sur les volumes qui se recouvrent. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environment")
	int32 Priority;

	/** Vrai si le point monde est dans la boîte (orientée et mise à l'échelle comme le composant). */
	UFUNCTION(BlueprintPure, Category = "Environment")
	bool ContainsPoint(const FVector& WorldLocation) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UBoxComponent> Box;
};
