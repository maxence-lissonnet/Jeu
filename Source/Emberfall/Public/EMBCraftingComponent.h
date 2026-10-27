// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "EMBItemTypes.h"
#include "EMBCraftingComponent.generated.h"

class UDataTable;
class UEMBEquipmentComponent;
class UEMBInventoryComponent;

/** La file de fabrication a changé (serveur, ou client propriétaire après réplication). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMBOnCraftingQueueChanged);

/** Une demande de fabrication a été refusée par le serveur (client propriétaire, non fiable). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnCraftRejected, FName, RecipeId, EEMBCraftResult, Reason);

/**
 * File de fabrication du joueur (référentiel section 14).
 *
 * ARCHITECTURE RÉSEAU
 *  - RequestCraft / RequestCancel sont des RPC Server validés ; le serveur revérifie tout (recette, glyphe,
 *    ingrédients, place dans le sac, taille de la file). Un refus est renvoyé au client par Client_CraftRejected.
 *  - Les ingrédients sont consommés à la mise en file (atomique) et remboursés à l'annulation.
 *  - La file est calculée par un timer serveur (pas de Tick) et répliquée au seul propriétaire. La progression
 *    affichée est déduite de l'heure serveur : les champs répliqués ne changent qu'à un démarrage, une pause ou une
 *    livraison (voir FEMBCraftQueueEntry).
 *  - Seule la première entrée progresse, et seulement si l'atelier requis (ligne DT_BuildPieces) est construit à
 *    600 cm ou moins ; sinon elle est en pause. Un exemplaire terminé mais non livrable (sac plein) attend de la place.
 *
 * Table : DT_Recipes (FEMBRecipeRow), chemin lu dans la configuration (section [/Script/Emberfall.EMBCraftingComponent]
 * de DefaultGame.ini, ajoutée par le chef d'équipe) ; défaut /Game/Data/DT_Recipes.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent), Config = Game)
class EMBERFALL_API UEMBCraftingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBCraftingComponent();

	// ---- Contrat (section 12) ----

	/** Met Count exemplaires de la recette en file (1 à MaxCraftCountPerRequest). Un Count hors bornes est rejeté par la validation du RPC. */
	UFUNCTION(BlueprintCallable, Server, Reliable, WithValidation, Category = "Crafting")
	void RequestCraft(FName RecipeId, int32 Count);

	/** Annule l'entrée d'index donné (RecipeId doit correspondre à l'entrée, la file a pu avancer) et rembourse les ingrédients des exemplaires restants. */
	UFUNCTION(BlueprintCallable, Server, Reliable, WithValidation, Category = "Crafting")
	void RequestCancel(int32 QueueIndex, FName RecipeId);

	void ExportSaveData(FEMBCraftingSaveData& Out) const;

	/** Serveur. Import BORNÉ : recettes inconnues ignorées, quantités et progression bornées, 10 entrées au plus. */
	void ImportSaveData(const FEMBCraftingSaveData& In);

	// ---- Interface ----

	/** Serveur : valide et met en file. Renvoie Ok ou la raison du refus. Aucune ressource n'est consommée en cas de refus. */
	EEMBCraftResult TryEnqueue(FName RecipeId, int32 Count);

	UFUNCTION(BlueprintPure, Category = "Crafting")
	TArray<FEMBCraftQueueEntry> GetQueue() const { return Queue; }

	/** Progression 0..1 de l'exemplaire en cours de l'entrée (calculée depuis l'heure serveur ; 0 pour une entrée hors tête de file). */
	UFUNCTION(BlueprintPure, Category = "Crafting")
	float GetEntryProgress(int32 QueueIndex) const;

	/** Vrai si l'entrée ne progresse pas (hors tête de file, atelier absent, ou en attente de place dans le sac). */
	UFUNCTION(BlueprintPure, Category = "Crafting")
	bool IsEntryPaused(int32 QueueIndex) const;

	/** Ligne DT_Recipes, ou nullptr. */
	const FEMBRecipeRow* FindRecipe(FName RecipeId) const;

	UPROPERTY(BlueprintAssignable, Category = "Crafting")
	FEMBOnCraftingQueueChanged OnCraftingQueueChanged;

	UPROPERTY(BlueprintAssignable, Category = "Crafting")
	FEMBOnCraftRejected OnCraftRejected;

	/** DataTable des recettes (lignes FEMBRecipeRow). Valeur de configuration (DefaultGame.ini, section de la classe) ; défaut /Game/Data/DT_Recipes. */
	UPROPERTY(Config, BlueprintReadOnly, Category = "Crafting")
	TSoftObjectPtr<UDataTable> RecipeTable;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Queue();

	UFUNCTION(Client, Unreliable)
	void Client_CraftRejected(FName RecipeId, EEMBCraftResult Reason);

private:
	bool HasAuthorityOwner() const;
	void EnsureRecipeTable();
	void ResolveDependencies();

	/** Heure serveur (identique sur le serveur et les clients, à la latence près). */
	double GetServerNow() const;

	/** Progression en secondes de l'exemplaire en cours à l'instant Now. */
	static float ComputeProgressSeconds(const FEMBCraftQueueEntry& Entry, double Now);

	bool IsGlyphUnlocked(FName GlyphId) const;
	bool IsStationAvailable(FName StationRow) const;

	/** Test d'atelier espacé (1 s) tant que la tête de file est en pause ; à chaque passage quand elle progresse. */
	bool IsStationAvailableThrottled(FName StationRow, double Now, bool bRunning);

	/** Limite de fréquence des RPC (~10/s) : faux = excédent à ignorer. */
	bool ConsumeRpcBudget();

	/** Contrôle de cohérence d'une ligne de recette (quantités positives et bornées). */
	bool IsRecipeUsable(const FEMBRecipeRow& Recipe, bool bCheckOutput) const;

	/** Ingrédients × Count ; faux si une quantité dépasse les bornes. */
	bool BuildScaledCost(const FEMBRecipeRow& Recipe, int32 Count, TMap<FName, int32>& OutCost) const;
	void RefundIngredients(const FEMBRecipeRow& Recipe, int32 Count);

	/** Livre un exemplaire terminé. Faux s'il ne peut pas l'être maintenant (sac plein) : l'exemplaire attend. */
	bool DeliverOneUnit(const FEMBRecipeRow& Recipe);

	/** Passage du timer serveur : progression, pauses, livraisons. */
	void ProcessQueue();
	void StartQueueTimerIfNeeded();
	void StopQueueTimer();
	void NotifyQueueChanged();

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> LoadedRecipes;

	UPROPERTY(ReplicatedUsing = OnRep_Queue)
	TArray<FEMBCraftQueueEntry> Queue;

	UPROPERTY(Transient)
	TObjectPtr<UEMBInventoryComponent> Inventory;

	UPROPERTY(Transient)
	TObjectPtr<UEMBEquipmentComponent> Equipment;

	FTimerHandle QueueTimerHandle;

	// Limitation des RPC et cache du test d'atelier (serveur).
	double RpcWindowStart = 0.0;
	int32 RpcWindowCount = 0;
	FName CachedStationName = NAME_None;
	bool bCachedStationResult = false;
	double LastStationCheckTime = -1000.0;
};
