// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/StreamableManager.h"
#include "EMBHitReceiver.h"
#include "EMBResourceNodeTypes.h"
#include "EMBResourceNode.generated.h"

class APawn;
class UPrimitiveComponent;
class UStaticMeshComponent;

/**
 * Nœud de ressource récoltable (arbre, rocher, filon, buisson, carcasse, source d'eau). Référentiel §13.
 *
 * Données : une ligne de DT_ResourceNodes (RowName), lue via UEMBResourceNodeSubsystem.
 * Coups : la lame des joueurs balaie EMBMeleeHit (ECC_GameTraceChannel1) ; le nœud y répond Overlap et reçoit le coup
 * par IEMBHitReceiver (UEMBCombatComponent::ApplyHitToActor). Seul le serveur traite le coup.
 * Distance : mesurée au volume englobant du mesh (ou à l'origine si plus proche), choix assumé : l'origine d'un grand
 * arbre ou d'un rocher est loin de sa surface.
 *
 * Réseau : l'état stable est un seul octet répliqué, NetState (1 disponible, 2 épuisé, 3 caché par le jour, 4 épuisé et
 * caché ; 0 = jamais reçu, jamais émis, donc la première valeur est toujours transmise). Les PV et les accumulateurs de
 * rendement restent sur le serveur. L'acteur n'est JAMAIS masqué ni sans collision au niveau de l'acteur (il
 * deviendrait non pertinent pour le réseau) : visibilité et collision se règlent sur chaque composant primitif,
 * y compris ceux ajoutés en Blueprint. Dormance (DORM_Initial) : chaque changement d'état réveille la réplication
 * (FlushNetDormancy + ForceNetUpdate), de sorte que des milliers de nœuds ne coûtent rien au repos.
 * Les nœuds dynamiques (carcasses) restent éveillés (courte durée de vie).
 *
 * Coût : aucun Tick ; les filons "nuit seulement" écoutent OnEnvironmentChanged (+ un contrôle de repli à 60 s) ; les
 * réapparitions sont gérées par un unique timer du sous-système.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBResourceNode : public AActor, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBResourceNode();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostActorCreated() override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
#if WITH_EDITOR
	/** Copier-coller / duplication dans l'éditeur : un nouvel identifiant évite deux nœuds avec le même NodeId. */
	virtual void PostEditImport() override;
#endif

	/** Serveur. Valide l'instigateur, l'outil et la distance, applique les dégâts de récolte et verse les ressources. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	/**
	 * Pourquoi un coup de ce pawn serait refusé (None = accepté). Sans effet de bord, utilisable côté client pour le
	 * retour d'interface (outil inadapté, tier insuffisant, nuit...). Charge la table au premier appel si besoin.
	 */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	EEMBHarvestReject GetRejectReason(const APawn* Harvester) const;

	// ---------------------------------------------------------------- Accès

	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	FName GetRowName() const { return RowName; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	FGuid GetNodeId() const { return NodeId; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	bool IsDepleted() const { return bDepleted; }

	/** Vrai si le nœud est visible et récoltable (ni épuisé, ni caché par le jour). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	bool IsAvailable() const { return !bDepleted && !bHiddenByNight; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Harvest")
	bool IsDynamicNode() const { return bDynamicNode; }

	UStaticMeshComponent* GetMeshComponent() const { return Mesh; }

	// ---------------------------------------------------------------- API du sous-système (serveur)

	/** Initialise un nœud dynamique AVANT FinishSpawning (appelé par SpawnDynamicNode). */
	void InitDynamicNode(FName InRowName);

	/** Remet le nœud à neuf (PV pleins, visible, collision rétablie). */
	void Respawn();

	/** Restaure l'état épuisé (chargement d'une sauvegarde) sans XP, sans chute et sans nouvelle planification. */
	void RestoreDepleted();

protected:
	/** Mise en scène : coup de récolte reçu (secousse, particules). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Harvest", meta = (DisplayName = "On Harvest Hit"))
	void BP_OnHarvestHit(FVector Location);

	/** Mise en scène : craquement de l'arbre épuisé avant sa chute. Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Harvest", meta = (DisplayName = "On Felling Crack"))
	void BP_OnFellingCrack(float DurationSeconds);

	/** Mise en scène : le nœud devient visible/récoltable ou ne l'est plus (épuisement, réapparition, jour/nuit). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Harvest", meta = (DisplayName = "On Availability Changed"))
	void BP_OnAvailabilityChanged(bool bAvailable);

	/** Cosmétique, non fiable : un coup de récolte a touché le nœud. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayHitFeedback(FVector_NetQuantize HitLocation);

	/** Cosmétique, non fiable : début du craquement (l'arbre tombe après DurationSeconds). */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayFellingCrack(float DurationSeconds);

	/**
	 * Cosmétique, non fiable : débris de fracture, chargés puis créés LOCALEMENT sur chaque machine (sauf serveur dédié),
	 * durée de vie 15 s. La classe de débris ne doit donc pas être répliquée.
	 */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_SpawnFracture(TSoftClassPtr<AActor> InFractureClass);

	UFUNCTION()
	void OnRep_NetState();

	/** Diffusé par UEMBEnvironmentSubsystem au passage jour/nuit : rafraîchit les filons nocturnes. */
	UFUNCTION()
	void HandleEnvironmentChanged();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Harvest")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** Ligne de DT_ResourceNodes (Pine, Oak, IronVein...). Répliquée une fois (nœuds dynamiques). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Replicated, Category = "Emberfall|Harvest")
	FName RowName = NAME_None;

	/** Identifiant stable des nœuds posés (clé de sauvegarde), généré dans l'éditeur. Vide pour un nœud dynamique. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Harvest")
	FGuid NodeId;

	/** 0 = jamais initialisé (jamais une valeur réelle) ; 1 disponible ; 2 épuisé ; 3 caché par le jour ; 4 épuisé et caché. */
	UPROPERTY(ReplicatedUsing = OnRep_NetState)
	uint8 NetState = 0;

private:
	/** Quantité due à un joueur pour une ressource : total des parts, et partie déjà versée en unités entières. */
	struct FHarvestAccumulator
	{
		TWeakObjectPtr<AActor> Owner; // PlayerState du récolteur (survit à la mort du pawn), sinon le pawn
		FName ResourceId = NAME_None;
		float Total = 0.f;
		int32 Paid = 0;
	};

	/** État d'origine d'un composant primitif (le nœud le coupe et le rétablit selon la disponibilité). */
	struct FComponentState
	{
		TWeakObjectPtr<UPrimitiveComponent> Component;
		TEnumAsByte<ECollisionEnabled::Type> Collision = ECollisionEnabled::NoCollision;
		bool bHiddenInGame = false;
	};

	/** Résultat du contrôle d'un coup : rendement et dégâts effectifs. */
	struct FResolvedHarvest
	{
		int32 Tier = 0;
		float Damage = 0.f;
		float Multiplier = 1.f;
	};

	const FEMBResourceNodeRow* FindRow() const;
	bool IsWithinHarvestRange(const APawn& Harvester) const;
	EEMBHarvestReject ResolveHarvest(const APawn* Harvester, const FEMBResourceNodeRow& Row, FResolvedHarvest& Out) const;
	void AccumulateYield(APawn& Harvester, const FEMBResourceNodeRow& Row, float Fraction, float Multiplier);
	/** Verse le reste dû (arrondi du total moins le déjà versé, par ressource) au récolteur qui épuise le nœud. */
	void FlushAccumulators(AActor* DepleterKey);
	/** Verse Amount à l'inventaire du pawn de Owner. Retourne false si aucun inventaire n'est joignable. */
	static bool PayResource(AActor* Owner, FName ResourceId, int32 Amount);

	/** Épuisement : XP, versement du reste, puis effet selon le type (chute d'arbre, fracture, simple disparition). */
	void BeginDepletion(APawn& Harvester, const FEMBResourceNodeRow& Row, const FVector& HitDirection);
	void FinishFelling();
	/** Cache le nœud, planifie la réapparition (nœud posé) ou la disparition (nœud dynamique). */
	void ConcludeDepletion();
	void SetDepletedState(bool bNewDepleted);
	/** Serveur : recalcule NetState, réveille la réplication et applique l'état localement. */
	void PushNetState();
	void ApplyAvailabilityState();
	void CacheComponentStates();
	void EnsureHitCollision();
	void RefreshNightState();
	void OnFractureClassLoaded();

	float Health = 0.f;
	bool bFelling = false;
	bool bDynamicNode = false;
	bool bDepleted = false;
	bool bHiddenByNight = false;
	bool bComponentsCached = false;

	FVector FellingDirection = FVector::ForwardVector;
	TWeakObjectPtr<AActor> FellingOwnerKey;
	TArray<FHarvestAccumulator> Accumulators;
	TArray<FComponentState> ComponentStates;

	FTimerHandle NightTimerHandle;
	FTimerHandle FellingTimerHandle;

	TSharedPtr<FStreamableHandle> FractureLoadHandle;
	TSharedPtr<FStreamableHandle> FallingLoadHandle;
	TSoftClassPtr<AActor> PendingFractureClass;
	TSoftClassPtr<AEMBFallingTree> PendingFallingClass;
};
