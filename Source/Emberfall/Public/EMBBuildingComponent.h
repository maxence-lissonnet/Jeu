// Emberfall - composant de construction du joueur (Dev B, contrat 3.6).

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"
#include "EMBBuildingTypes.h"
#include "EMBBuildingComponent.generated.h"

class AEMBBuildablePiece;
class UDataTable;
class UEMBInventoryComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UPrimitiveComponent;
class UStaticMeshComponent;

/** Le mode construction est entré ou quitté (local au joueur). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnBuildModeChanged, bool, bInBuildMode);

/** Le résultat de placement de l'aperçu a changé, ou le serveur a répondu à une demande de pose. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnPlacementResult, EEMBPlacementResult, Result, float, PredictedIntegrity);

/**
 * Composant de construction à placer sur le personnage joueur.
 *
 * Architecture :
 *  - Client propriétaire : mode construction, aperçu fantôme, accrochage, vérifications (retour visuel immédiat).
 *  - Serveur : Server_PlacePiece refait TOUTE la vérification (jamais de confiance au client), consomme les
 *    ressources de façon atomique, fait apparaître la pièce.
 *  - Le client n'envoie que le nom de ligne de la DataTable et un Transform : ni coût, ni classe, ni intégrité.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent))
class EMBERFALL_API UEMBBuildingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBBuildingComponent();

	// ----- API figée (contrat 3.6) -----

	UFUNCTION(BlueprintCallable, Category = "Building")
	void ToggleBuildMode();

	/** Sélectionne une ligne de DT_BuildPieces. */
	UFUNCTION(BlueprintCallable, Category = "Building")
	void SetSelectedPiece(FName PieceRowName);

	/** Direction : +1 / -1. Pas de 90 degrés (15 pour le mobilier). */
	UFUNCTION(BlueprintCallable, Category = "Building")
	void RotatePreview(float Direction);

	/** Demande la pose de la pièce à l'emplacement de l'aperçu (RPC serveur). */
	UFUNCTION(BlueprintCallable, Category = "Building")
	void ConfirmPlacement();

	UFUNCTION(BlueprintCallable, Category = "Building")
	void CancelBuildMode();

	UFUNCTION(BlueprintPure, Category = "Building")
	bool IsInBuildMode() const { return bBuildMode; }

	// ----- Compléments -----

	UFUNCTION(BlueprintPure, Category = "Building")
	FName GetSelectedPiece() const { return SelectedRowName; }

	UFUNCTION(BlueprintPure, Category = "Building")
	const FEMBPlacementEvaluation& GetLastEvaluation() const { return LastEvaluation; }

	UPROPERTY(BlueprintAssignable, Category = "Building")
	FEMBOnBuildModeChanged OnBuildModeChanged;

	UPROPERTY(BlueprintAssignable, Category = "Building")
	FEMBOnPlacementResult OnPlacementResult;

	/**
	 * Une pièce est débloquée si sa ligne n'exige aucun glyphe (RequiredGlyph vide), ou si le composant de progression
	 * du PlayerState (AEMBPlayerState) possède ce glyphe. Sans PlayerState Emberfall, tout est disponible (avertissement
	 * unique dans le journal). Appelée côté client (aperçu) ET côté serveur (validation) : le serveur fait foi.
	 * Virtuelle : une classe dérivée peut ajouter d'autres conditions.
	 */
	virtual bool IsPieceUnlocked(FName PieceRowName) const;

	/**
	 * Serveur : multiplicateur de coût (passif « Bâtisseur » : 1 - 0,10 x rang). Répliqué au seul propriétaire pour que
	 * l'aperçu du client calcule le même coût que le serveur. Borné à [0,1 ; 1].
	 */
	UFUNCTION(BlueprintCallable, Category = "Building")
	void SetBuildCostMultiplier(float Multiplier);

	UFUNCTION(BlueprintPure, Category = "Building")
	float GetBuildCostMultiplier() const { return BuildCostMultiplier; }

	/**
	 * Coût effectif d'une pièce : chaque quantité x multiplicateur, arrondie au SUPÉRIEUR, minimum 1. Même calcul
	 * pour l'aperçu et pour la validation/consommation serveur.
	 */
	TMap<FName, int32> GetEffectiveCost(const FEMBBuildPieceRow& Row) const;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ----- RPC -----

	/**
	 * Demande de pose. Reliable : une pose perdue serait incompréhensible pour le joueur ; la fréquence est faible
	 * (limitée par MinServerPlaceInterval). La validation ne rejette (et donc ne déconnecte) que les données
	 * manifestement forgées (NaN, noms vides, coordonnées hors monde) ; tout le reste passe par la vérification
	 * complète de l'implémentation, qui répond par Client_PlacementResult.
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_PlacePiece(FName PieceRowName, const FTransform& Transform);

	/** Retour du serveur vers le joueur (message d'erreur / confirmation). */
	UFUNCTION(Client, Reliable)
	void Client_PlacementResult(EEMBPlacementResult Result, float PredictedIntegrity);

private:
	// ----- Données -----

	void LoadBuildTable();
	bool FindRowCopy(FName RowName, FEMBBuildPieceRow& OutRow) const;
	bool IsLocalOwner() const;

	// ----- Mode construction -----

	void EnterBuildMode();
	void ExitBuildMode();
	void RefreshPreviewPieceClass();

	// ----- Aperçu (client) -----

	void UpdatePreview();
	bool ComputeDesiredTransform(const AEMBBuildablePiece* PieceCDO, FTransform& OutTransform) const;
	bool TrySnapToSockets(const FVector& CursorPoint, const AEMBBuildablePiece* PieceCDO, FVector& OutLocation, double& OutYaw) const;
	void EnsureGhost();
	void ApplyGhost(const FEMBPlacementEvaluation& Evaluation);
	FLinearColor ComputeGhostColor(const FEMBPlacementEvaluation& Evaluation) const;

	// ----- Vérifications (partagées client/serveur) -----

	/** Normalise un Transform reçu : rotation en lacet seul, quantifiée au pas de la pièce, échelle 1. */
	FTransform SanitizeTransform(const FEMBBuildPieceRow& Row, const FTransform& InTransform) const;

	/**
	 * Évaluation complète d'un placement.
	 * @param bPreview vrai : aperçu local (accroche le Z au sol, aucune tolérance de distance).
	 *                 faux : validation serveur (vérifie le Z, tolère ServerDistanceTolerance de latence).
	 */
	FEMBPlacementEvaluation EvaluatePlacement(const FEMBBuildPieceRow& Row, const AEMBBuildablePiece* PieceCDO, const FTransform& DesiredTransform, bool bPreview) const;

	EEMBPlacementResult EvaluateGround(const FEMBBuildPieceRow& Row, const AEMBBuildablePiece* PieceCDO, bool bSnapZ, FTransform& InOutTransform, bool& bOutTerrain, TArray<const AActor*>& OutGroundActors) const;

	bool IsNoBuildZone(AActor* Actor, const UPrimitiveComponent* Component) const;

	/** Terrain = ALandscapeProxy (détecté par la hiérarchie de classes, sans dépendance au module Landscape) ou acteur portant TerrainActorTag. */
	bool IsTerrainActor(const AActor* Actor) const;

	/** Pas de grille de la catégorie (300 fondation/sol/toit, 150 mur/pilier/escalier, 25 mobilier/poste). */
	double GetGridStep(const FEMBBuildPieceRow& Row) const;

	/** Vrai si le pivot (XY) coïncide, à la tolérance près, avec une position d'accrochage valide sur un socket voisin. */
	bool IsPivotOnSocketCandidate(const AEMBBuildablePiece* PieceCDO, const FTransform& InTransform) const;

	UEMBInventoryComponent* GetInventory() const;

	/** Résout la classe d'une ligne (chargement synchrone la première fois, puis cache du moteur). */
	UClass* ResolvePieceClass(const FEMBBuildPieceRow& Row) const;

public:
	// ----- Réglages (valeurs par défaut = section 5 du référentiel) -----

	/**
	 * DataTable DT_BuildPieces (ligne = FEMBBuildPieceRow). Si elle est vide, le composant utilise la table Config du
	 * sous-système de sauvegarde (UEMBSaveSubsystem::GetConfiguredBuildPiecesTable) : une seule source de vérité.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Data")
	TSoftObjectPtr<UDataTable> BuildPiecesTable;

	/** Pièce présélectionnée à la première entrée en mode construction (ligne de DT_BuildPieces). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Data")
	FName DefaultPieceRowName = TEXT("Wood_Foundation");

	/** Matériau translucide de l'aperçu ; doit exposer le paramètre vectoriel GhostColorParameterName. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	TSoftObjectPtr<UMaterialInterface> GhostMaterial;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	FName GhostColorParameterName = TEXT("GhostColor");

	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	FLinearColor ColorGreen = FLinearColor(0.1f, 0.8f, 0.2f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	FLinearColor ColorYellow = FLinearColor(0.95f, 0.85f, 0.1f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	FLinearColor ColorOrange = FLinearColor(1.f, 0.45f, 0.05f, 1.f);

	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview")
	FLinearColor ColorRed = FLinearColor(0.9f, 0.05f, 0.05f, 1.f);

	/** Fréquence de mise à jour de l'aperçu (traces + vérifications). 30 Hz suffit et économise les requêtes physiques. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview", meta = (ClampMin = "5", ClampMax = "120"))
	float PreviewUpdateHz = 30.f;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float MaxPlacementDistance = EMBBuildingDefaults::MaxPlacementDistance;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float GridSize = EMBBuildingDefaults::GridSize;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float SocketSnapRadius = EMBBuildingDefaults::SocketSnapRadius;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float OverlapBoxShrink = EMBBuildingDefaults::OverlapBoxShrink;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float GroundTraceDepth = EMBBuildingDefaults::GroundTraceDepth;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float MaxFoundationSlopeDeg = EMBBuildingDefaults::MaxFoundationSlopeDeg;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	float FurnitureSnap = EMBBuildingDefaults::FurnitureSnap;

	/** Tolérance de distance côté serveur (cm) : absorbe la latence entre la position du client et celle du serveur. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Server", meta = (ClampMin = "0"))
	float ServerDistanceTolerance = EMBBuildingDefaults::ServerDistanceTolerance;

	/**
	 * Délai minimal entre deux requêtes de pose reçues (anti-spam). Le chronomètre est relancé à CHAQUE requête reçue,
	 * même rejetée : un client qui inonde le serveur reste bloqué tant qu'il inonde, et ne reçoit aucune réponse.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Server", meta = (ClampMin = "0"))
	float MinServerPlaceInterval = 0.1f;

	/** Anti-rebond côté client : délai minimal entre deux ConfirmPlacement (évite le double-clic accidentel). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Preview", meta = (ClampMin = "0"))
	float ConfirmDebounceTime = 0.15f;

	/** Limites (section 5) : pièces par base (composante connexe) et par monde. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Server", meta = (ClampMin = "1"))
	int32 MaxPiecesPerBase = EMBBuildingDefaults::MaxPiecesPerBase;

	UPROPERTY(EditDefaultsOnly, Category = "Building|Server", meta = (ClampMin = "1"))
	int32 MaxPiecesPerWorld = EMBBuildingDefaults::MaxPiecesPerWorld;

	/** Tag d'acteur identifiant un terrain (en plus des ALandscapeProxy). Seul un terrain compte comme "sol". */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	FName TerrainActorTag = TEXT("Terrain");

	/** Canal du tir de caméra de l'aperçu. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collision")
	TEnumAsByte<ECollisionChannel> PlacementTraceChannel = ECC_Visibility;

	/** Canal des tirs verticaux de recherche de sol. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collision")
	TEnumAsByte<ECollisionChannel> GroundTraceChannel = ECC_Visibility;

	/**
	 * Canal des tests de chevauchement : EMBBuilding = ECC_GameTraceChannel2 (référentiel 4.3). Les pièces le BLOQUENT ;
	 * les volumes "zone interdite" le traversent en OVERLAP.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collision")
	TEnumAsByte<ECollisionChannel> PlacementOverlapChannel = ECC_GameTraceChannel2;

	/** Tag d'acteur ou de composant marquant une zone interdite (en plus du Gameplay Tag Building.NoBuildZone). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Rules")
	FName NoBuildActorTag = TEXT("NoBuild");

private:
	bool bBuildMode = false;
	FName SelectedRowName = NAME_None;
	FEMBBuildPieceRow CachedRow;
	bool bHasCachedRow = false;

	/** Évite de relancer (et de journaliser) à 30 Hz le chargement d'une classe de pièce invalide. */
	bool bPreviewClassFailed = false;

	/** Lacet utilisateur (relatif à la pièce visée quand on s'accroche). */
	double PreviewYaw = 0.0;

	FEMBPlacementEvaluation LastEvaluation;

	bool bHasBroadcastResult = false;
	EEMBPlacementResult LastBroadcastResult = EEMBPlacementResult::InvalidPiece;
	int32 LastBroadcastIntegrity = -1;

	FLinearColor LastGhostColor = FLinearColor::Transparent;

	/** Dernier instant de réception d'une requête de pose (serveur), acceptée ou non. */
	double LastServerRequestTime = -1000.0;

	UPROPERTY(Replicated)
	float BuildCostMultiplier = 1.f;

	/** Dernier ConfirmPlacement local (anti-rebond). */
	double LastConfirmTime = -1000.0;

	/** Inventaire du propriétaire, résolu une fois (évite FindComponentByClass à 30 Hz). */
	mutable TWeakObjectPtr<UEMBInventoryComponent> CachedInventory;

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> LoadedTable;

	UPROPERTY(Transient)
	TObjectPtr<UClass> PreviewPieceClass;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> GhostMesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GhostMID;
};
