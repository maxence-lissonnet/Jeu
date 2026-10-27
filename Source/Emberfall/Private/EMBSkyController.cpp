// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBSkyController.h"
#include "Emberfall.h"
#include "EMBWeatherSubsystem.h"
#include "EMBWeatherTypes.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"

namespace
{
	// Réglages visuels par état météo. Valeurs de conception, à ajuster dans l'éditeur de niveau avec le rendu réel.

	/** Part de la lumière directe qui traverse le ciel (nuages, pluie, cendres). */
	float SkySunTransmittance(EEMBWeatherState State)
	{
		switch (State)
		{
		case EEMBWeatherState::Clear:         return 1.f;
		case EEMBWeatherState::Overcast:      return 0.5f;
		case EEMBWeatherState::Rain:          return 0.35f;
		case EEMBWeatherState::Storm:         return 0.2f;
		case EEMBWeatherState::Fog:           return 0.4f;
		case EEMBWeatherState::Snow:          return 0.45f;
		case EEMBWeatherState::CorruptedMist: return 0.35f;
		case EEMBWeatherState::AshStorm:      return 0.25f;
		default:                              return 1.f;
		}
	}

	/** Multiplicateur de densité du brouillard. */
	float SkyFogMultiplier(EEMBWeatherState State)
	{
		switch (State)
		{
		case EEMBWeatherState::Clear:         return 1.f;
		case EEMBWeatherState::Overcast:      return 1.5f;
		case EEMBWeatherState::Rain:          return 2.f;
		case EEMBWeatherState::Storm:         return 3.f;
		case EEMBWeatherState::Fog:           return 8.f;
		case EEMBWeatherState::Snow:          return 3.f;
		case EEMBWeatherState::CorruptedMist: return 6.f;
		case EEMBWeatherState::AshStorm:      return 5.f;
		default:                              return 1.f;
		}
	}

	/** Couverture nuageuse, de 0 (dégagé) à 1 (couvert). */
	float SkyCloudCoverage(EEMBWeatherState State)
	{
		switch (State)
		{
		case EEMBWeatherState::Clear:         return 0.2f;
		case EEMBWeatherState::Overcast:      return 0.7f;
		case EEMBWeatherState::Rain:          return 0.85f;
		case EEMBWeatherState::Storm:         return 1.f;
		case EEMBWeatherState::Fog:           return 0.5f;
		case EEMBWeatherState::Snow:          return 0.8f;
		case EEMBWeatherState::CorruptedMist: return 0.7f;
		case EEMBWeatherState::AshStorm:      return 0.9f;
		default:                              return 0.2f;
		}
	}

	/** Échelle de la diffusion de Mie de l'atmosphère (voile) : plus élevée sous cendres et brume. */
	float SkyAtmosphereHaze(EEMBWeatherState State)
	{
		switch (State)
		{
		case EEMBWeatherState::Fog:           return 2.f;
		case EEMBWeatherState::CorruptedMist: return 2.5f;
		case EEMBWeatherState::AshStorm:      return 3.f;
		case EEMBWeatherState::Storm:         return 1.5f;
		default:                              return 1.f;
		}
	}

	float SkyBlendByState(float (*Getter)(EEMBWeatherState), EEMBWeatherState From, EEMBWeatherState To, float Alpha)
	{
		return FMath::Lerp(Getter(From), Getter(To), Alpha);
	}

	/**
	 * Applique NewValue seulement si elle s'écarte de la dernière valeur appliquée de plus de ~1 % (les réglages
	 * de rendu invalident l'état de rendu : inutile de les réécrire à 10 Hz pour des variations imperceptibles).
	 * Last < 0 signifie « jamais appliquée » ; les valeurs pilotées sont toutes positives ou nulles.
	 */
	template <typename TApply>
	void SkyApplyIfChanged(float& Last, float NewValue, TApply Apply)
	{
		const float Threshold = FMath::Max(0.01f * FMath::Max(FMath::Abs(Last), FMath::Abs(NewValue)), 1e-4f);
		if (Last < 0.f || FMath::Abs(NewValue - Last) > Threshold)
		{
			Last = NewValue;
			Apply(NewValue);
		}
	}

	/** Premier composant du type donné trouvé sur l'acteur (nullptr si l'acteur est invalide). */
	template <typename TComponent>
	TComponent* SkyFindComponent(AActor* Actor)
	{
		return IsValid(Actor) ? Actor->FindComponentByClass<TComponent>() : nullptr;
	}
}

AEMBSkyController::AEMBSkyController()
{
	PrimaryActorTick.bCanEverTick = false; // Timer à 10 Hz plutôt que Tick.
	bReplicates = false;                   // Pure présentation locale.

	bAutoFindActors = true;
	UpdatesPerSecond = 10.f;
	SunAzimuthDegrees = 0.f;
	SunIntensityDay = 10.f;
	MoonIntensityNight = 0.25f;
	SkyLightIntensityDay = 1.f;
	SkyLightIntensityNight = 0.15f;
	SkyRecaptureIntervalSeconds = 30.f;
	BaseFogDensity = 0.02f;
	CloudCoverageParameter = TEXT("CloudCoverage");

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AEMBSkyController::BeginPlay()
{
	Super::BeginPlay();

	UWorld* World = GetWorld();
	// Un serveur dédié n'a ni écran ni ciel à piloter.
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	ResolveReferences();
	World->GetTimerManager().SetTimer(UpdateTimerHandle, this, &AEMBSkyController::UpdateSky, 1.f / FMath::Max(1.f, UpdatesPerSecond), true);
	UpdateSky();
}

void AEMBSkyController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(UpdateTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void AEMBSkyController::ResolveReferences()
{
	AActor* Sun = SunLight.Get();
	AActor* Moon = MoonLight.Get();
	AActor* SkyLightActor = SkyLight.Get();
	AActor* Atmosphere = SkyAtmosphere.Get();
	AActor* Fog = HeightFog.Get();
	AActor* Clouds = VolumetricCloud.Get();

	// Recherche unique dans le niveau pour les références vides (un seul parcours des acteurs, au démarrage).
	UWorld* World = GetWorld();
	if (bAutoFindActors && World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Sun && Actor != Moon && SkyFindComponent<UDirectionalLightComponent>(Actor))
			{
				Sun = Actor;
			}
			if (!SkyLightActor && SkyFindComponent<USkyLightComponent>(Actor))
			{
				SkyLightActor = Actor;
			}
			if (!Atmosphere && SkyFindComponent<USkyAtmosphereComponent>(Actor))
			{
				Atmosphere = Actor;
			}
			if (!Fog && SkyFindComponent<UExponentialHeightFogComponent>(Actor))
			{
				Fog = Actor;
			}
			if (!Clouds && SkyFindComponent<UVolumetricCloudComponent>(Actor))
			{
				Clouds = Actor;
			}
		}
	}

	SunComponent = SkyFindComponent<UDirectionalLightComponent>(Sun);
	MoonComponent = SkyFindComponent<UDirectionalLightComponent>(Moon);
	SkyLightComponent = SkyFindComponent<USkyLightComponent>(SkyLightActor);
	AtmosphereComponent = SkyFindComponent<USkyAtmosphereComponent>(Atmosphere);
	FogComponent = SkyFindComponent<UExponentialHeightFogComponent>(Fog);
	CloudComponent = SkyFindComponent<UVolumetricCloudComponent>(Clouds);

	// Valeur d'origine de l'atmosphère (réglée dans le niveau) : la météo la multiplie sans l'écraser.
	if (AtmosphereComponent)
	{
		BaseMieScale = AtmosphereComponent->MieScatteringScale;
	}
	// Un SkyLight non Movable ignore les changements d'intensité à l'exécution.
	if (SkyLightComponent && SkyLightComponent->Mobility != EComponentMobility::Movable && !bWarnedSkyLightMobility)
	{
		bWarnedSkyLightMobility = true;
		UE_LOG(LogEmberfall, Warning, TEXT("AEMBSkyController: le SkyLight '%s' doit etre en mobilite Movable pour que son intensite change."), *GetNameSafe(SkyLightComponent->GetOwner()));
	}

	// Les nuages se règlent par un paramètre du matériau : une instance dynamique créée une seule fois.
	if (CloudComponent && CloudComponent->Material && !CloudCoverageParameter.IsNone())
	{
		CloudMaterial = UMaterialInstanceDynamic::Create(CloudComponent->Material, this);
		CloudComponent->SetMaterial(CloudMaterial);
	}
}

void AEMBSkyController::AimLight(UDirectionalLightComponent* Light, const FVector& LightDirection, bool& bWarned)
{
	if (!Light)
	{
		return;
	}
	if (Light->Mobility != EComponentMobility::Movable)
	{
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogEmberfall, Warning, TEXT("AEMBSkyController: la lumiere '%s' doit etre en mobilite Movable pour tourner."), *GetNameSafe(Light->GetOwner()));
		}
		return;
	}
	if (AActor* LightOwner = Light->GetOwner())
	{
		LightOwner->SetActorRotation(LightDirection.Rotation());
	}
}

void AEMBSkyController::UpdateSky()
{
	UWorld* World = GetWorld();
	const UEMBWeatherSubsystem* Weather = World ? World->GetSubsystem<UEMBWeatherSubsystem>() : nullptr;
	if (!World || !Weather)
	{
		return;
	}

	// Point de vue du joueur local (la météo est celle du biome où il regarde / se trouve).
	FVector ViewLocation = GetActorLocation();
	if (const APlayerController* Controller = World->GetFirstPlayerController())
	{
		FRotator ViewRotation;
		Controller->GetPlayerViewPoint(ViewLocation, ViewRotation);
	}

	EEMBWeatherState From = EEMBWeatherState::Clear;
	EEMBWeatherState To = EEMBWeatherState::Clear;
	float Alpha = 0.f;
	Weather->GetWeatherBlendAt(ViewLocation, From, To, Alpha);
	const float Hours = Weather->GetTimeOfDayHours();

	// ---- Position du soleil. Angle sur l'arc : 0 au lever (6 h), pi au coucher (20 h), puis pi -> 2 pi sous l'horizon la nuit.
	const float DayHours = EMBSurvivalDefaults::NightStartHour - EMBSurvivalDefaults::NightEndHour;
	const float NightHours = 24.f - DayHours;
	float SinceDawn = FMath::Fmod(Hours - EMBSurvivalDefaults::NightEndHour, 24.f);
	if (SinceDawn < 0.f)
	{
		SinceDawn += 24.f;
	}
	const float ArcAngle = SinceDawn < DayHours
		? PI * SinceDawn / DayHours
		: PI + PI * (SinceDawn - DayHours) / NightHours;

	// Direction vers le soleil ; la lumière voyage en sens inverse. La lune est à l'opposé du soleil.
	FVector ToSun(FMath::Cos(ArcAngle), 0.f, FMath::Sin(ArcAngle));
	ToSun = FRotator(0.f, SunAzimuthDegrees, 0.f).RotateVector(ToSun);
	const float SunElevation = ToSun.Z; // sinus de l'élévation : > 0 le jour, < 0 la nuit

	const float SunTransmittance = SkyBlendByState(&SkySunTransmittance, From, To, Alpha);
	const float SunFade = FMath::SmoothStep(-0.02f, 0.2f, SunElevation);
	const float MoonFade = FMath::SmoothStep(-0.02f, 0.2f, -SunElevation);

	if (SunComponent)
	{
		AimLight(SunComponent, -ToSun, bWarnedSunMobility);
		SkyApplyIfChanged(LastSunIntensity, SunIntensityDay * SunFade * SunTransmittance, [this](float Value) { SunComponent->SetIntensity(Value); });
	}
	if (MoonComponent)
	{
		AimLight(MoonComponent, ToSun, bWarnedMoonMobility); // la lune est en -ToSun, sa lumière voyage vers +ToSun
		SkyApplyIfChanged(LastMoonIntensity, MoonIntensityNight * MoonFade * SunTransmittance, [this](float Value) { MoonComponent->SetIntensity(Value); });
	}

	// ---- Lumière ambiante : jour/nuit, assombrie par la météo. Recapture limitée (coûteuse).
	if (SkyLightComponent)
	{
		const float Ambient = FMath::Lerp(SkyLightIntensityNight, SkyLightIntensityDay, SunFade) * FMath::Lerp(0.5f, 1.f, SunTransmittance);
		SkyApplyIfChanged(LastSkyLightIntensity, Ambient, [this](float Value) { SkyLightComponent->SetIntensity(Value); });

		const double Now = World->GetTimeSeconds();
		if (Now - LastRecaptureTime >= SkyRecaptureIntervalSeconds)
		{
			LastRecaptureTime = Now;
			SkyLightComponent->RecaptureSky();
		}
	}

	// ---- Brouillard, nuages, voile atmosphérique
	if (FogComponent)
	{
		SkyApplyIfChanged(LastFogDensity, BaseFogDensity * SkyBlendByState(&SkyFogMultiplier, From, To, Alpha), [this](float Value) { FogComponent->SetFogDensity(Value); });
	}
	if (CloudMaterial)
	{
		SkyApplyIfChanged(LastCloudCoverage, SkyBlendByState(&SkyCloudCoverage, From, To, Alpha), [this](float Value) { CloudMaterial->SetScalarParameterValue(CloudCoverageParameter, Value); });
	}
	if (AtmosphereComponent)
	{
		SkyApplyIfChanged(LastMieScale, BaseMieScale * SkyBlendByState(&SkyAtmosphereHaze, From, To, Alpha), [this](float Value) { AtmosphereComponent->SetMieScatteringScale(Value); });
	}
}
