// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBDragonArena.generated.h"

class AEMBDragonBoss;

/**
 * État de l'arène pour UNE phase du combat (index 0 = phase 1, 1 = phase 2, 2 = phase 3).
 * Les acteurs sont référencés par soft pointer : en World Partition, un acteur non chargé ne bloque pas le
 * chargement du niveau de l'arène (et ne produit pas de référence dure inter-cellules).
 */
USTRUCT(BlueprintType)
struct EMBERFALL_API FEMBArenaPhaseConfig
{
	GENERATED_BODY()

	/** Acteurs rendus visibles, solides et actifs à l'entrée dans la phase (ponts, passerelles, piliers de renfort...). */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Arène")
	TArray<TSoftObjectPtr<AActor>> ActorsToActivate;

	/** Acteurs détruits à l'entrée dans la phase (piliers de basalte, ponts effondrés, pourtour de la caldera). */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Arène")
	TArray<TSoftObjectPtr<AActor>> ActorsToDestroy;

	/** Zones de lave activées (visibles ET dangereuses : 25 dégâts/s). Leur emprise est celle des bornes de l'acteur. */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Arène")
	TArray<TSoftObjectPtr<AActor>> LavaZonesToActivate;

	/** Zones de lave éteintes (refroidies ou recouvertes). */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "Arène")
	TArray<TSoftObjectPtr<AActor>> LavaZonesToDeactivate;
};

/**
 * Arène évolutive du combat contre Vaelthyrax (caldera de Cendrehaute).
 *
 * S'abonne à AEMBDragonBoss::OnPhaseChanged (serveur ET clients : le délégué est aussi diffusé par le RepNotify de
 * la phase) et applique localement l'état de l'arène : activation / destruction d'acteurs par phase, zones de lave.
 * L'arène n'est PAS répliquée : chaque machine applique le même état déterministe à partir de la phase répliquée
 * du boss, ce qui évite de répliquer des dizaines d'acteurs de décor et gère la reconnexion en cours de combat
 * (rattrapage des phases manquées à la liaison).
 *
 * Foudre (phase 3) : un éclair toutes les 3,0 s sur un joueur vivant tiré au hasard, décalé de 0 à 400 cm ;
 * marqueur au sol 1,5 s (diffusé par le boss, Multicast_ShowGroundMarker), puis impact 150 (foudre, rayon 250 cm),
 * coup normal donc esquivable. Serveur uniquement (jamais sur un client).
 *
 * Lave : dégâts 25/s appliqués par UN timer serveur (4 ticks/s, 6,25 par tick), jamais par Tick. Un joueur est
 * touché s'il se trouve dans les bornes d'une zone active ; les dégâts passent par UEMBCombatComponent::ApplyHitToActor.
 *
 * Alternative World Partition (non implémentée ici, signatures non vérifiées pour 5.4) : regrouper les décors de
 * chaque phase dans des Data Layers runtime et basculer leur état via le UDataLayerManager du monde
 * (UDataLayerManager::GetDataLayerManager(this), puis SetDataLayerRuntimeState / SetDataLayerInstanceRuntimeState
 * avec EDataLayerRuntimeState::Activated / Unloaded). Avantage : le streaming gère le chargement et la mémoire de
 * grands décors ; inconvénient : état asynchrone (il faut attendre le chargement) et dépendance au World Partition.
 * Pour une arène de cette taille, l'activation d'acteurs ci-dessous reste plus simple et plus prévisible.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBDragonArena : public AActor
{
	GENERATED_BODY()

public:
	AEMBDragonArena();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Lie l'arène à un boss (appelé automatiquement au BeginPlay ; public pour un boss instancié plus tard). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Arena")
	void BindToBoss(AEMBDragonBoss* Boss);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Arena")
	int32 GetAppliedPhase() const { return AppliedPhase; }

protected:
	/** Mise en scène (cinématique, tremblement, VFX, audio) : appelé sur serveur et clients après l'application de la phase. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Arena", meta = (DisplayName = "On Arena Phase Applied"))
	void OnArenaPhaseApplied(int32 Phase);

	/**
	 * Tempête de cendres de la phase 3 : mise en scène uniquement (VFX, post-process, audio), sur serveur et clients.
	 * Aucune règle de jeu n'y est attachée ; les éclairs, eux, sont gérés en C++ (StrikeLightning).
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Arena", meta = (DisplayName = "On Ash Storm Changed"))
	void OnAshStormChanged(bool bActive);

	/** Mise en scène de l'état final calme (boss vaincu) : serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Arena", meta = (DisplayName = "On Arena Final Calm State"))
	void OnArenaFinalCalmState();

	UFUNCTION()
	void HandleBossPhaseChanged(int32 NewPhase, int32 OldPhase);

	UFUNCTION()
	void HandleEncounterEnded();

	/** Boss de l'arène (instance du niveau). Si vide, le premier AEMBDragonBoss du monde est utilisé. */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TSoftObjectPtr<AEMBDragonBoss> BossActor;

	/** États par phase : index 0 = phase 1, 1 = phase 2, 2 = phase 3. */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TArray<FEMBArenaPhaseConfig> PhaseConfigs;

	/** Dégâts de lave par seconde (docs/00 section 6 : 25). */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Arena|Lave", meta = (ClampMin = "0.0"))
	float LavaDamagePerSecond = 25.f;

	/** Intervalle des ticks de lave (s) : 0,25 = 4 ticks/s. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Arena|Lave", meta = (ClampMin = "0.05"))
	float LavaTickInterval = 0.25f;

	/** Tolérance verticale (cm) autour des bornes d'une zone de lave pour détecter un joueur qui s'y tient. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Arena|Lave", meta = (ClampMin = "0.0"))
	float LavaVerticalTolerance = 250.f;

private:
	struct FLavaZone
	{
		TWeakObjectPtr<AActor> Actor;
		FBox Bounds = FBox(ForceInit);
	};

	void TryBindToBoss();
	void ApplyPhaseEntry(int32 Phase);
	void ActivateArenaActor(AActor* Actor);
	void RemoveArenaActor(AActor* Actor);
	void RegisterLavaZone(AActor* Actor);
	void UnregisterLavaZone(AActor* Actor);
	void UpdateLavaTimer();
	void TickLava();

	// Foudre de phase 3 (serveur uniquement) : un éclair toutes les 3,0 s sur un joueur vivant au hasard.
	void StartLightning();
	void StrikeLightning();
	void ResolveLightningImpact(FVector Location);

	void EnterFinalCalmState();

	TWeakObjectPtr<AEMBDragonBoss> BoundBoss;
	bool bFinalCalm = false;
	TArray<FLavaZone> ActiveLavaZones;
	int32 AppliedPhase = 0;

	FTimerHandle LavaTimerHandle;
	FTimerHandle BossSearchTimerHandle;
	FTimerHandle LightningTimerHandle;
	TArray<FTimerHandle> LightningImpactTimers;
	FRandomStream RandomStream;
};
