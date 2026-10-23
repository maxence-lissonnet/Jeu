// Emberfall - pièce de construction répliquée (Dev B).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBBuildingTypes.h"
#include "EMBHitReceiver.h"
#include "EMBBuildablePiece.generated.h"

class UStaticMeshComponent;
class UNiagaraSystem;

/**
 * Pièce de construction posée dans le monde (fondation, mur, toit...).
 *
 * Réseau (CRITIQUE avec des milliers de pièces) :
 *  - Une pièce ne bouge jamais : SetReplicateMovement(false), aucun Tick.
 *  - Dormance : l'acteur est mis en DORM_DormantAll après sa réplication initiale. Une base de joueurs peut compter
 *    plusieurs milliers de pièces ; sans dormance, le serveur évaluerait chaque pièce à chaque cycle de réplication
 *    (comparaison des propriétés par connexion). Dormant, une pièce ne coûte plus rien au CPU serveur ni à la bande
 *    passante tant que rien ne change. Toute modification d'une propriété répliquée DOIT être suivie de
 *    FlushNetDormancy(), sinon les clients ne la verront jamais.
 *  - NetCullDistance réduite : un client ne reçoit que les pièces proches de lui.
 *  - Propriétés d'identité (matériau, catégorie...) en COND_InitialOnly : envoyées une seule fois.
 */
UCLASS()
class EMBERFALL_API AEMBBuildablePiece : public AActor, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBBuildablePiece();

	// ----- Initialisation (serveur, AVANT FinishSpawning avec SpawnActorDeferred) -----

	/**
	 * Renseigne l'identité de la pièce depuis sa ligne de DataTable. Les valeurs partent dans le bunch initial.
	 * Appeler cette fonction marque la pièce comme "posée par un joueur" (pas de délai de grâce d'intégrité).
	 * @param InPlacerPlayerId APlayerState::GetPlayerId() du poseur (INDEX_NONE si inconnu).
	 */
	void InitializePiece(FName InRowName, EEMBBuildMaterial InMaterial, EEMBPieceCategory InCategory, bool bInAnchoredToGround, int32 InPlacerPlayerId = INDEX_NONE);

	/** Identifiant de joueur du poseur (répliqué une fois). Sert à la sauvegarde / attribution future des bases. */
	int32 GetPlacerPlayerId() const { return PlacerPlayerId; }

	/**
	 * Identifiant STABLE du poseur (UEMBSaveSubsystem::GetStablePlayerId), serveur uniquement, non répliqué.
	 * PlacerPlayerId change à chaque session ; cet identifiant survit aux sauvegardes.
	 */
	const FString& GetPlacerStableId() const { return PlacerStableId; }
	void SetPlacerStableId(const FString& InStableId) { PlacerStableId = InStableId; }

	/** Serveur, avant FinishSpawning : restaure l'état d'une pièce sauvegardée (PV, poseur). */
	void RestoreFromSave(float InHealth, const FString& InPlacerStableId);

	/** Serveur : vrai si la pièce a été posée en jeu (faux : pré-placée dans le niveau ou streamée -> délai de grâce 1,0 s). */
	bool WasPlacedByPlayer() const { return bPlacedByPlayer; }

	// ----- Accesseurs -----

	FName GetPieceRowName() const { return PieceRowName; }
	EEMBBuildMaterial GetMaterial() const { return Material; }
	EEMBPieceCategory GetCategory() const { return Category; }
	bool IsAnchoredToGround() const { return bAnchoredToGround; }
	bool IsCollapsed() const { return bCollapsed; }
	float GetCurrentHealth() const { return CurrentHealth; }
	float GetMaxHealth() const;
	/** Intégrité courante 0..100 (valeur répliquée, utilisée pour le retour visuel et la prédiction d'aperçu). */
	float GetIntegrity() const { return Integrity; }
	UStaticMeshComponent* GetMeshComponent() const { return Mesh; }

	/** Demi-dimensions de la boîte de placement/chevauchement (espace local de la pièce). */
	const FVector& GetPlacementExtent() const { return PlacementExtent; }
	/** Décalage du centre de la boîte par rapport au pivot (convention : pivot au centre-bas de la pièce). */
	const FVector& GetPlacementBoxCenterOffset() const { return PlacementBoxCenterOffset; }

	/** Boîte englobante monde (alignée sur les axes) pour un Transform donné. Utilisable sur le CDO pour l'aperçu. */
	FBox GetBoundsForTransform(const FTransform& InTransform) const;
	/** Boîte englobante monde de la pièce posée. */
	FBox GetStructureBounds() const { return GetBoundsForTransform(GetActorTransform()); }

	/** Sockets "Snap_*" en espace local (mesh + ExtraSnapSockets), mis en cache. Fonctionne sur le CDO. */
	void GetSnapSocketsLocal(FEMBSnapSocketArray& OutSockets) const;
	/** Sockets "Snap_*" en espace monde. */
	void GetSnapSocketsWorld(FEMBSnapSocketArray& OutSockets) const;

	/** Voisins en contact structurel (le graphe est tenu par UEMBStructureSubsystem, source unique de vérité). */
	void GetSupportNeighbors(TArray<AEMBBuildablePiece*>& OutNeighbors) const;

	// ----- Dégâts et effondrement (serveur) -----

	/** Applique des dégâts structurels (ex. créature ou dragon territorial en raid). Retourne les dégâts effectifs. À 0 PV : Collapse(). */
	float ApplyStructuralDamage(float Amount, AActor* DamageCauser);

	/** Détruit la pièce via le subsystem (mise à jour du graphe + effondrement en cascade des pièces non soutenues). */
	void Collapse();

	/** Écriture serveur de l'intégrité (appelée par le subsystem). Réveille la dormance seulement si la valeur change. */
	void SetIntegrityServer(float NewIntegrity);

	/**
	 * Exécution effective de l'effondrement (collision coupée, effets, destruction différée).
	 * Appelée par le subsystem APRÈS retrait du nœud du graphe ; ne pas appeler directement (utiliser Collapse()).
	 * @param bHeavyDebris vrai : autorisé à produire des débris coûteux (GeometryCollection / physique).
	 */
	void ExecuteCollapse(bool bHeavyDebris);

	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;

	/**
	 * IEMBHitReceiver : les coups des créatures et des dragons territoriaux en raid abîment les constructions
	 * (les arènes de boss sont des zones interdites à la construction). Les joueurs ne détruisent pas leurs propres
	 * pièces à l'arme, sauf si bAllowPlayerStructureDamage est activé.
	 * Multiplicateur par type : physique x1,0 ; feu x1,5 sur le bois et x0,5 sinon ; autres types x0,5.
	 * Retourne Killed si la pièce est détruite par le coup, sinon Hit (Ignored si déjà effondrée / hors serveur).
	 */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

protected:
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Integrity();

	UFUNCTION()
	void OnRep_CurrentHealth();

	UFUNCTION()
	void OnRep_Collapsed();

	/** Effet visuel d'effondrement, rejoué chez tous (non fiable : purement cosmétique). */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayCollapseEffects(bool bHeavyDebris);

	/** Pousse l'intégrité et les PV vers le matériau via Custom Primitive Data (voir .cpp). */
	void UpdateVisualState();

	/** Coupe la collision (hors simulation physique en cours). */
	void ApplyCollapsedCollision();

	/** Après une écriture de propriété répliquée sur un acteur dormant : réveil + mise à jour réseau immédiate. */
	void WakeForReplication();

protected:
	// ----- Composants -----

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Building")
	TObjectPtr<UStaticMeshComponent> Mesh;

	// ----- Réglages de classe (Blueprint) -----

	/** Demi-dimensions de la boîte de chevauchement (cm). Défaut : fondation 300x300x50. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Placement")
	FVector PlacementExtent = FVector(150.0, 150.0, 25.0);

	/** Décalage du centre de la boîte depuis le pivot. Défaut : pivot au centre-bas d'une fondation de 50 cm. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Placement")
	FVector PlacementBoxCenterOffset = FVector(0.0, 0.0, 25.0);

	/** Sockets supplémentaires (en plus des sockets "Snap_*" du StaticMesh). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Placement")
	TArray<FEMBSnapSocket> ExtraSnapSockets;

	/**
	 * Option serveur pour un futur mode PvP : si faux (défaut, jeu coopératif PvE), les coups portés par un pawn
	 * contrôlé par un joueur sont ignorés par les pièces.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Stats")
	bool bAllowPlayerStructureDamage = false;

	/** Multiplicateur de PV par rapport à la valeur du matériau (ex. 0.5 pour un pilier fin). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Stats", meta = (ClampMin = "0.01"))
	float HealthMultiplier = 1.f;

	/** Distance de culling réseau (cm). Au-delà, la pièce n'est plus répliquée vers ce client. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Network", meta = (ClampMin = "1000"))
	float NetCullDistance = 15000.f;

	/**
	 * La pièce participe-t-elle à la navigation (NavMesh) ? Par défaut oui : les murs doivent bloquer l'IA.
	 * Attention : des milliers de pièces qui modifient le NavMesh coûtent cher. Recommandation projet : régler le
	 * NavMesh en Runtime Generation = "Dynamic" avec mise à jour regroupée (Project Settings > Navigation Mesh >
	 * "Min time between nav updates"), et désactiver ce réglage pour le mobilier / les petites pièces.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Navigation")
	bool bAffectNavigation = true;

	/** Délai avant destruction de l'acteur après effondrement (laisse les clients jouer l'effet). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collapse", meta = (ClampMin = "0.1"))
	float CollapseLifetime = 2.5f;

	/** Durée de vie des débris physiques / GeometryCollection côté client. */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collapse", meta = (ClampMin = "0.5"))
	float DebrisLifetime = 6.f;

	/** Simule la physique du mesh à l'effondrement (nécessite une collision simple sur le StaticMesh). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collapse")
	bool bSimulatePhysicsOnCollapse = true;

	/**
	 * Acteur de débris optionnel (Blueprint dérivé de AGeometryCollectionActor, Chaos).
	 * Coût : une GeometryCollection fracturée active des dizaines de corps Chaos par pièce ; elle n'est donc
	 * produite que pour un nombre limité de pièces par vague (voir UEMBStructureSubsystem) et jamais sur serveur dédié.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collapse")
	TSoftClassPtr<AActor> CollapseDebrisClass;

	/** Nuage de poussière Niagara optionnel (peu coûteux, joué pour toutes les pièces). */
	UPROPERTY(EditDefaultsOnly, Category = "Building|Collapse")
	TSoftObjectPtr<UNiagaraSystem> CollapseDustEffect;

	// ----- État répliqué -----

	/** Ligne de DT_BuildPieces d'origine. Envoyée une seule fois. */
	UPROPERTY(Replicated)
	FName PieceRowName = NAME_None;

	/** EditAnywhere : permet aussi de régler une pièce posée à la main dans le niveau (ruines préconstruites). */
	UPROPERTY(EditAnywhere, Replicated, Category = "Building|Stats")
	EEMBBuildMaterial Material = EEMBBuildMaterial::Wood;

	UPROPERTY(EditAnywhere, Replicated, Category = "Building|Stats")
	EEMBPieceCategory Category = EEMBPieceCategory::Wall;

	/** Vrai pour les fondations posées sur le terrain (intégrité 100). Fixé par InitializePiece côté serveur. */
	UPROPERTY(EditAnywhere, Replicated, Category = "Building|Stats")
	bool bAnchoredToGround = false;

	/** PV courants (le maximum se déduit du matériau : pas répliqué). */
	UPROPERTY(ReplicatedUsing = OnRep_CurrentHealth)
	float CurrentHealth = 0.f;

	/** Intégrité 0..100 calculée par le serveur (UEMBStructureSubsystem). Répliquée pour le retour visuel et l'aperçu. */
	UPROPERTY(ReplicatedUsing = OnRep_Integrity)
	float Integrity = 0.f;

	UPROPERTY(ReplicatedUsing = OnRep_Collapsed)
	bool bCollapsed = false;

	/** Joueur poseur (APlayerState::GetPlayerId). Envoyé une seule fois. */
	UPROPERTY(Replicated)
	int32 PlacerPlayerId = INDEX_NONE;

	/** Serveur seulement : posée par un joueur (vrai) ou pré-placée / streamée (faux). */
	bool bPlacedByPlayer = false;

	/** Serveur seulement : identifiant stable du poseur (sauvegarde). */
	FString PlacerStableId;

	/** Cache des sockets locaux (le mesh d'une classe ne change pas à l'exécution). */
	mutable FEMBSnapSocketArray CachedLocalSockets;
	mutable bool bLocalSocketsCached = false;
};
