// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "GameFramework/Actor.h"
#include "EMBSkyController.generated.h"

class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UMaterialInstanceDynamic;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UVolumetricCloudComponent;

/**
 * Pilote du ciel (client), contrat 15. À placer UNE fois dans le niveau : il règle les acteurs de ciel du niveau
 * (soleil, lune optionnelle, SkyLight, SkyAtmosphere, ExponentialHeightFog, VolumetricCloud) selon l'heure et la
 * météo au point de vue du joueur local, 10 fois par seconde.
 *
 *  - Aucun asset obligatoire : une référence vide est simplement ignorée. Si bAutoFindActors est vrai, les
 *    acteurs manquants sont cherchés dans le niveau au démarrage (premier DirectionalLight = soleil ; la lune
 *    n'est jamais devinée).
 *  - Les lumières pilotées doivent être en mobilité MOBILE (Movable), sinon la rotation est refusée (avertissement
 *    unique dans le journal).
 *  - Aucune logique de jeu : lecture seule de UEMBWeatherSubsystem. Inactif sur serveur dédié.
 *  - Les nuages sont pilotés par un paramètre scalaire du matériau du VolumetricCloud (CloudCoverageParameter) :
 *    un matériau qui n'expose pas ce paramètre reste inchangé.
 */
UCLASS()
class EMBERFALL_API AEMBSkyController : public AActor
{
	GENERATED_BODY()

public:
	AEMBSkyController();

	// ---------------------------------------------------------------- Références (acteurs du niveau)

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.DirectionalLight"))
	TSoftObjectPtr<AActor> SunLight;

	/** Optionnelle : éclaire la nuit à l'opposé du soleil. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.DirectionalLight"))
	TSoftObjectPtr<AActor> MoonLight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.SkyLight"))
	TSoftObjectPtr<AActor> SkyLight;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.SkyAtmosphere"))
	TSoftObjectPtr<AActor> SkyAtmosphere;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.ExponentialHeightFog"))
	TSoftObjectPtr<AActor> HeightFog;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References", meta = (AllowedClasses = "/Script/Engine.VolumetricCloud"))
	TSoftObjectPtr<AActor> VolumetricCloud;

	/** Cherche au démarrage les acteurs de ciel non renseignés. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|References")
	bool bAutoFindActors;

	// ---------------------------------------------------------------- Paramètres

	/** Fréquence de mise à jour (Hz). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "1", ClampMax = "30"))
	float UpdatesPerSecond;

	/** Rotation de l'arc du soleil autour de l'axe vertical, en degrés (oriente le lever et le coucher). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters")
	float SunAzimuthDegrees;

	/** Intensité du soleil en plein jour par temps clair. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "0"))
	float SunIntensityDay;

	/** Intensité de la lune en pleine nuit par temps clair. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "0"))
	float MoonIntensityNight;

	/** Intensité du SkyLight le jour et la nuit. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "0"))
	float SkyLightIntensityDay;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "0"))
	float SkyLightIntensityNight;

	/** Intervalle minimal entre deux recaptures du SkyLight (coûteuses), en secondes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "1"))
	float SkyRecaptureIntervalSeconds;

	/** Densité de base du brouillard par temps clair (multipliée selon la météo). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters", meta = (ClampMin = "0"))
	float BaseFogDensity;

	/** Paramètre scalaire du matériau de nuages piloté par la couverture nuageuse (0 = dégagé, 1 = couvert). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky|Parameters")
	FName CloudCoverageParameter;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Résout les références (soft + recherche automatique) et met en cache les composants. */
	void ResolveReferences();
	void UpdateSky();

	/** Rotation unique de lumière : refus signalé une seule fois si la lumière n'est pas Movable. */
	void AimLight(UDirectionalLightComponent* Light, const FVector& LightDirection, bool& bWarned);

	UPROPERTY(Transient) TObjectPtr<UDirectionalLightComponent> SunComponent;
	UPROPERTY(Transient) TObjectPtr<UDirectionalLightComponent> MoonComponent;
	UPROPERTY(Transient) TObjectPtr<USkyLightComponent> SkyLightComponent;
	UPROPERTY(Transient) TObjectPtr<USkyAtmosphereComponent> AtmosphereComponent;
	UPROPERTY(Transient) TObjectPtr<UExponentialHeightFogComponent> FogComponent;
	UPROPERTY(Transient) TObjectPtr<UVolumetricCloudComponent> CloudComponent;
	UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;

	FTimerHandle UpdateTimerHandle;
	double LastRecaptureTime = -1000.0;
	bool bWarnedSunMobility = false;
	bool bWarnedMoonMobility = false;
	bool bWarnedSkyLightMobility = false;

	/** Échelle de Mie de l'atmosphère telle que réglée dans le niveau : la météo la multiplie, ne la remplace pas. */
	float BaseMieScale = 1.f;

	/** Dernières valeurs appliquées (-1 = jamais) : on ne réécrit que si l'écart dépasse ~1 %. */
	float LastSunIntensity = -1.f;
	float LastMoonIntensity = -1.f;
	float LastSkyLightIntensity = -1.f;
	float LastFogDensity = -1.f;
	float LastCloudCoverage = -1.f;
	float LastMieScale = -1.f;
};
