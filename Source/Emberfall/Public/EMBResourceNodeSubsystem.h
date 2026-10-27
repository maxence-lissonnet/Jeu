// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TimerManager.h"
#include "Engine/StreamableManager.h"
#include "EMBResourceNodeTypes.h"
#include "EMBResourceNodeSubsystem.generated.h"

class AEMBFallingTree;
class AEMBResourceNode;
class UDataTable;
class UStaticMesh;

/**
 * Sous-système de récolte (serveur) : registre des nœuds, réapparitions, plafonds d'arbres qui tombent, carcasses
 * dynamiques et sauvegarde. Référentiel §12 et §13.
 *
 * Réapparitions : UN seul timer (1 s) parcourt les nœuds épuisés ; il n'existe aucun timer par nœud. Un nœud n'entre
 * dans le registre des réapparitions que lorsqu'il est épuisé : le coût est proportionnel aux nœuds épuisés, pas aux
 * milliers de nœuds du monde. Une réapparition est reportée de 30 s si un joueur ou une pièce est à moins de 300 cm.
 *
 * Streaming : l'état épuisé (restauré d'une sauvegarde ou en cours) est indexé par NodeId ; un nœud qui se charge plus
 * tard (World Partition) reçoit son état à l'enregistrement. Un nœud déchargé garde son entrée ; si son délai expire
 * pendant qu'il est déchargé, l'entrée disparaît et il se rechargera intact.
 *
 * La table est lue au premier usage serveur (chargement synchrone, table de quelques lignes). Son chemin se règle dans
 * DefaultGame.ini, section [/Script/Emberfall.EMBResourceNodeSubsystem], clé NodesTable.
 */
UCLASS(Config = Game)
class EMBERFALL_API UEMBResourceNodeSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UEMBResourceNodeSubsystem();

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	/** Raccourci d'accès depuis n'importe quel objet du monde. */
	static UEMBResourceNodeSubsystem* Get(const UObject* WorldContextObject);

	// ---------------------------------------------------------------- Contrat §12

	/**
	 * Serveur. Crée un nœud dynamique (carcasse de créature) à partir d'une ligne de DT_ResourceNodes. Durée de vie
	 * LifeSpanSeconds (<= 0 : 600 s). Non sauvegardé. Retourne nullptr hors serveur ou si la ligne est inconnue.
	 */
	AEMBResourceNode* SpawnDynamicNode(FName NodeRowId, const FTransform& Transform, float LifeSpanSeconds);

	void ExportSaveData(FEMBResourceNodesSaveData& Out) const;

	/** Serveur. Applique l'état aux nœuds présents et garde le reste en attente de streaming. */
	void ImportSaveData(const FEMBResourceNodesSaveData& In);

	// ---------------------------------------------------------------- API des nœuds (serveur)

	/**
	 * Charge la table au premier appel (synchrone, quelques lignes) et lance le préchargement asynchrone des classes
	 * NodeClass des carcasses, pour qu'aucun chargement n'ait lieu à la mort d'une créature. Idempotent.
	 */
	void EnsureTableLoaded();

	/** Ligne de la table (nullptr si inconnue). Le pointeur reste valide tant que la table est chargée. */
	const FEMBResourceNodeRow* FindRow(FName RowName) const;

	void RegisterNode(AEMBResourceNode* Node);
	void UnregisterNode(AEMBResourceNode* Node);

	/** Planifie la réapparition d'un nœud posé épuisé. */
	void ScheduleRespawn(AEMBResourceNode* Node, float Seconds);

	/**
	 * Crée l'arbre qui tombe. Retourne nullptr (le nœud disparaît alors sans chute) si le plafond de 3 chutes par joueur
	 * ou de 12 au total est atteint. OwnerKey identifie le joueur responsable (PlayerState de préférence).
	 */
	AEMBFallingTree* SpawnFallingTree(TSubclassOf<AEMBFallingTree> TreeClass, UStaticMesh* TreeMesh,
		const FTransform& Transform, const FVector& FallDirection, AActor* OwnerKey);

	/** Nombre de nœuds posés actuellement épuisés (diagnostic). */
	int32 GetPendingRespawnCount() const { return Respawns.Num(); }

private:
	struct FRespawnEntry
	{
		double RespawnAtSeconds = 0.0;
		TWeakObjectPtr<AEMBResourceNode> Node; // invalide tant que le nœud n'est pas (re)chargé
	};

	struct FFallingEntry
	{
		TWeakObjectPtr<AEMBFallingTree> Tree;
		TWeakObjectPtr<AActor> OwnerKey;
	};

	void StartRespawnTimer();
	void PollRespawns();
	bool IsRespawnBlocked(const AEMBResourceNode& Node) const;
	void PurgeFallingTrees();

	/** Chemin de la DataTable DT_ResourceNodes (structure FEMBResourceNodeRow). */
	UPROPERTY(Config)
	TSoftObjectPtr<UDataTable> NodesTable;

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> LoadedTable;

	bool bTableLoadAttempted = false;

	/** Garde les classes NodeClass en mémoire (préchargées au chargement de la table). */
	TArray<TSharedPtr<FStreamableHandle>> NodeClassHandles;

	TMap<FGuid, TWeakObjectPtr<AEMBResourceNode>> PlacedNodes;
	TMap<FGuid, FRespawnEntry> Respawns;
	TArray<TWeakObjectPtr<AEMBResourceNode>> DynamicNodes;
	TArray<FFallingEntry> FallingTrees;

	FTimerHandle RespawnTimerHandle;
};
