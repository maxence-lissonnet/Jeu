// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBCreatureSpawner.generated.h"

class AEMBCreatureBase;

/**
 * Point d'apparition de créatures (référentiel §16). Acteur placé dans le niveau, SERVEUR UNIQUEMENT (non répliqué,
 * non chargé chez les clients). Un spawner gère un archétype : une AEMBCreatureBase (goule, wyverneau) ou le
 * Sylvadrac (AEMBTerritorialDragon).
 *
 * Règles :
 *  - une vague n'apparaît que si un joueur vivant est à <= ActivationDistance (8 000 cm) du spawner ET qu'aucun n'est à
 *    < MinPlayerDistance (2 500 cm) du point d'apparition choisi ;
 *  - au plus MaxAlive créatures vivantes ; une vague contient GroupSizeMin..GroupSizeMax créatures de la ligne de
 *    données (goules : 2 à 4), groupées autour d'un point d'ancrage navigable ;
 *  - après une vague, la suivante attend RespawnInterval ; après une mort, RespawnInterval compté depuis la mort
 *    (le Sylvadrac impose son propre délai de 144 min via NotifyChildDied) ;
 *  - la nuit, une créature "corrompue" (bNightBoost de la ligne) double son poids d'apparition par un seul levier :
 *    MaxAlive x 2, intervalle inchangé (UEMBWeatherSubsystem::IsNight) ;
 *  - aucune vague si la place libre (ou le budget) est inférieure à GroupSizeMin ; un archétype boss est limité à 1 ;
 *  - budget serveur : jamais plus de 40 créatures vivantes dans le monde (compteur de UEMBCreatureSignificance) ;
 *  - une créature rendue au spawner (trop loin de tout joueur) libère son emplacement sans délai.
 *
 * Évaluation par un timer à 0,5 Hz (EvaluationInterval) : aucun Tick.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBCreatureSpawner : public AActor
{
	GENERATED_BODY()

public:
	AEMBCreatureSpawner();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Une créature (ou le dragon) du spawner vient de mourir : son emplacement est libéré et la prochaine vague attend
	 * OverrideRespawnDelay secondes (valeur négative : RespawnInterval, divisé par 2 la nuit pour les corrompus).
	 */
	void NotifyChildDied(AActor* Child, float OverrideRespawnDelay = -1.f);

	/** Une créature a été rendue au spawner (trop loin des joueurs) : emplacement libéré, aucun délai. */
	void NotifyChildReturned(AActor* Child);

	/**
	 * Configure un spawner créé par le code (Sylvadrac posé à la main : il crée ce spawner à sa mort pour programmer sa
	 * réapparition). À appeler entre SpawnActorDeferred et FinishSpawning.
	 */
	void InitializeAsRespawner(TSubclassOf<AActor> InArchetypeClass, float DelaySeconds, float InActivationDistance, float InMinPlayerDistance);

	int32 GetAliveCount() const { return Alive.Num(); }

protected:
	/** Archétype : AEMBCreatureBase (ou classe fille) ou AEMBTerritorialDragon. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner")
	TSubclassOf<AActor> ArchetypeClass;

	/** Nombre maximal de créatures vivantes de ce spawner. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "1"))
	int32 MaxAlive = 4;

	/** (*) Intervalle (s) entre deux vagues et après une mort. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "1.0"))
	float RespawnInterval = 180.f;

	/** Un joueur vivant doit être à cette distance (cm) du spawner pour qu'une vague apparaisse. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "0.0"))
	float ActivationDistance = 8000.f;

	/** Aucun joueur ne doit être à moins de cette distance (cm) du point d'apparition. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "0.0"))
	float MinPlayerDistance = 2500.f;

	/** Rayon (cm) autour du spawner où le point d'ancrage de la vague est tiré (0 : exactement sur le spawner). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "0.0"))
	float SpawnRadius = 1500.f;

	/** (*) Rayon (cm) de dispersion des membres d'un groupe autour du point d'ancrage. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "0.0"))
	float GroupSpread = 300.f;

	/** (*) Période (s) de l'évaluation des conditions d'apparition. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Spawner", meta = (ClampMin = "0.5"))
	float EvaluationInterval = 2.f;

private:
	void ResolveArchetype();
	void EvaluateSpawn();
	void PruneAlive();
	bool IsNightNow() const;
	float GetEffectiveInterval() const;
	bool HasPlayerWithin(const TArray<APawn*>& Players, const FVector& Center, float Radius) const;
	bool PickSpawnLocation(const FVector& Center, float Radius, FVector& OutLocation) const;
	AActor* SpawnOne(const FVector& Location);

	/** Créatures vivantes issues de ce spawner (références faibles : un acteur détruit est purgé). */
	TArray<TWeakObjectPtr<AActor>> Alive;

	FTimerHandle EvaluateTimerHandle;

	/** Heure monde avant laquelle aucune vague ne peut apparaître. */
	double NextSpawnTime = 0.0;

	// Données de l'archétype (lues dans la ligne de données si c'est une créature légère)
	int32 GroupSizeMin = 1;
	int32 GroupSizeMax = 1;
	bool bNightBoost = false;
	bool bArchetypeIsCreature = false;
};
