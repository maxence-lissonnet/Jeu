// Emberfall - graphe de structure, intégrité et effondrement (Dev B).

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TimerManager.h"
#include "EMBBuildingTypes.h"
#include "EMBStructureSubsystem.generated.h"

class AEMBBuildablePiece;

/** Description d'une pièce hypothétique (pas encore posée) pour prédire son intégrité. */
struct FEMBStructureProbe
{
	/** Boîte englobante monde de la pièce hypothétique. */
	FBox WorldBounds = FBox(ForceInit);
	EEMBBuildMaterial Material = EEMBBuildMaterial::Wood;
	/** Pilier : décroissance verticale x0,5. */
	bool bIsPillar = false;
};

/**
 * Graphe de structure d'un monde.
 *
 * ÉVOLUTION PRÉVUE (ADR v1 : on garde UN acteur dormant par pièce pour le vertical slice) :
 *   à terme, passage à un acteur "Base" unique par composante connexe, avec un Fast Array répliqué (FFastArraySerializer)
 *   décrivant les pièces et un rendu par Instanced Static Meshes (ISM/HISM), voir docs/03_Scalabilite.md.
 *   Le graphe ci-dessous (FNode, index spatial, intégrité, cascade) est DÉJÀ indépendant des acteurs : il ne
 *   dépend de AEMBBuildablePiece que pour lire les propriétés de la pièce à l'enregistrement et pour écrire
 *   l'intégrité (WriteBackIntegrity) et déclencher l'effondrement visuel (ExecuteCollapse). Remplacer ces trois points
 *   suffira à brancher le graphe sur l'acteur Base. Aucune refonte n'est nécessaire.
 *
 * Rôles :
 *  - Serveur ET clients : registre des pièces + adjacence (index spatial). Les clients s'en servent pour la
 *    prédiction d'intégrité de l'aperçu (ils lisent l'intégrité répliquée des voisins).
 *  - Serveur uniquement : calcul d'intégrité, détection des pièces non soutenues, effondrement en cascade.
 *
 * Modèle d'intégrité (section 5 du référentiel) :
 *   Intégrité(pièce) = max sur ses supports de [ Intégrité(support) - Décroissance(pièce, direction) ]
 *   Les fondations ancrées au sol valent 100. Décroissance verticale si Δz(centre pièce - centre support) > 50 cm,
 *   horizontale sinon ; la décroissance utilise le matériau de la pièce elle-même ; pilier : vertical x0,5.
 *
 * Complexité :
 *   Comme les décroissances sont strictement positives, l'intégrité vaut 100 - (plus court chemin pondéré depuis
 *   une fondation) : c'est un Dijkstra (valeur maximale, tas max).
 *   - Chaque pièce mémorise son SUPPORT PARENT (arbre des plus courts chemins).
 *   - Ajout d'une pièce : relaxation incrémentale limitée aux seules pièces dont l'intégrité AUGMENTE
 *     (ajouter un noeud ne peut qu'améliorer les chemins). Coût ~ O(k log k), k = pièces améliorées.
 *   - Retrait d'un lot de pièces (suppression dynamique d'un sous-graphe dépendant) : seules les pièces dont le chemin
 *     optimal passait par une pièce retirée (sous-arbre) sont recalculées : on les remet à zéro, on les réamorce depuis
 *     leurs voisins NON affectés (dont l'intégrité est inchangée, un retrait ne pouvant que la dégrader) puis un
 *     Dijkstra local les résout. Coût O(s log s), s = taille du sous-arbre dépendant, indépendant de la taille de la base.
 *   - Cascade : le recalcul initial découvre TOUTES les pièces privées de support ; elles sont ordonnées par niveau
 *     (profondeur BFS depuis la cassure) puis détruites par vagues, sans nouveau recalcul (une pièce non soutenue
 *     ne soutient personne : la retirer ne change l'intégrité d'aucune autre pièce).
 *   - Requêtes de voisinage : index spatial par cellules de 6 m, O(1) amorti par pièce.
 */
UCLASS(Config = Game)
class EMBERFALL_API UEMBStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** Raccourci d'accès depuis n'importe quel objet du monde. */
	static UEMBStructureSubsystem* Get(const UObject* WorldContextObject);

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	virtual void Deinitialize() override;

	// ----- Enregistrement -----

	/** Ajoute la pièce au graphe (clients et serveur). Sur le serveur : calcule son intégrité et propage. */
	void RegisterPiece(AEMBBuildablePiece* Piece);

	/**
	 * Retire la pièce du graphe sans la détruire (appelé depuis EndPlay).
	 * @param bRecomputeNeighbors vrai : le serveur recalcule le sous-graphe dépendant (destruction en cours de partie).
	 */
	void UnregisterPiece(AEMBBuildablePiece* Piece, bool bRecomputeNeighbors);

	/**
	 * Serveur : demande la destruction d'une pièce. Les demandes d'une même frame sont REGROUPÉES et traitées en fin de
	 * frame (début du tick suivant) par UN retrait de lot et UN recalcul : un souffle de dragon qui tue 50 pièces ne
	 * coûte pas 50 recalculs.
	 */
	void RequestCollapse(AEMBBuildablePiece* Piece);

	/**
	 * Serveur : retire immédiatement ces pièces du graphe, déclenche leur effondrement visuel, recalcule le sous-graphe
	 * dépendant et programme l'effondrement en cascade des pièces qui perdent leur support.
	 */
	void CollapsePiecesNow(const TArray<AEMBBuildablePiece*>& Pieces);

	// ----- Chargement de sauvegarde -----

	/**
	 * Serveur : ouvre un chargement en masse. Tant qu'il est ouvert, RegisterPiece construit l'adjacence mais ne
	 * calcule aucune intégrité : pas de propagation par pièce (qui coûterait O(n) fois une relaxation) et surtout aucun
	 * effondrement prématuré d'un mur enregistré avant sa fondation.
	 */
	void BeginBulkLoad();

	/**
	 * Serveur : clôt le chargement et lance UN recalcul global de l'intégrité depuis toutes les ancres, en
	 * O((V+E) log V). Les pièces d'intégrité <= 0 sont détruites SANS effet ni vague visible : au chargement personne ne
	 * regarde la scène, et des effondrements en cascade au démarrage seraient trompeurs.
	 * @return nombre de pièces détruites parce que non soutenues.
	 */
	int32 EndBulkLoad();

	bool IsBulkLoading() const { return bBulkLoading; }

	/** Toutes les pièces enregistrées (sauvegarde). Ordre non défini. */
	void GetAllPieces(TArray<AEMBBuildablePiece*>& OutPieces) const;

	// ----- Requêtes -----

	/**
	 * Prédit l'intégrité d'une pièce non posée à partir de ses voisins en contact (lit l'intégrité répliquée des pièces).
	 * Ne gère pas l'ancrage au sol : l'appelant le traite (une pièce ancrée vaut GroundedIntegrity).
	 * @param OutSupportCount nombre de pièces porteuses en contact (0 => aucun support => NoSupport).
	 * @return intégrité prédite (0 si aucun support valide).
	 */
	float PredictIntegrity(const FEMBStructureProbe& Probe, int32* OutSupportCount = nullptr) const;

	/**
	 * Nombre de pièces de la ou des bases (composantes connexes) que toucherait la sonde, interrompu dès que Limit est
	 * dépassé (coût borné par Limit). Sert à appliquer la limite de 3 000 pièces par base.
	 */
	int32 CountPiecesInTouchedBases(const FEMBStructureProbe& Probe, int32 Limit) const;

	/**
	 * Identifiant de base (composante connexe) d'une pièce enregistrée, stable d'une session à l'autre tant que la base
	 * ne change pas : hachage de l'angle minimal de la composante. INDEX_NONE si la pièce n'est pas enregistrée.
	 * Prévu pour la sauvegarde future. Coût O(taille de la base) : ne pas appeler à chaque frame.
	 */
	int32 GetBaseId(const AEMBBuildablePiece* Piece) const;

	/** Pièces dont la boîte englobante intersecte la boîte donnée. */
	void QueryPiecesInBox(const FBox& Box, TArray<AEMBBuildablePiece*>& OutPieces) const;

	/** Pièces dont la boîte englobante est à moins de Radius du point. */
	void QueryPiecesInSphere(const FVector& Center, float Radius, TArray<AEMBBuildablePiece*>& OutPieces) const;

	/** Voisins en contact structurel d'une pièce enregistrée. */
	void GetNeighbors(const AEMBBuildablePiece* Piece, TArray<AEMBBuildablePiece*>& OutNeighbors) const;

	int32 GetNumPieces() const { return PieceToNode.Num(); }

	/** Nombre de pièces en attente d'effondrement (diagnostic). */
	int32 GetPendingCollapseCount() const { return CollapseQueue.Num(); }

	/** Recouvrement minimal (cm) sur deux axes pour qu'un contact compte comme "par une face". */
	UPROPERTY(Config)
	float MinContactSize = EMBBuildingDefaults::MinContactSizeDefault;

private:
	using FNodeIdArray = TArray<int32, TInlineAllocator<16>>;

	/** Noeud du graphe : données compactes, sans accès aux UObject dans les boucles chaudes. */
	struct FNode
	{
		TWeakObjectPtr<AEMBBuildablePiece> Piece;
		FBox Bounds = FBox(ForceInit);
		float CenterZ = 0.f;
		/** Intégrité de travail (autorité serveur). */
		float Integrity = 0.f;
		EEMBBuildMaterial Material = EEMBBuildMaterial::Wood;
		TArray<int32, TInlineAllocator<8>> Neighbors;
		/** Support parent dans l'arbre des plus courts chemins (INDEX_NONE : ancrée ou non soutenue). */
		int32 Parent = INDEX_NONE;
		/** Profondeur BFS depuis la cassure, utilisée pour ordonner la cascade (0 = voisin direct). */
		int32 Depth = 0;
		/** Marqueur de visite (évite un TSet) ; mutable car posé par des requêtes const. Voir NextStamp(). */
		mutable uint32 VisitStamp = 0;
		bool bInUse = false;
		/** Ancrée au sol : intégrité fixe de 100. */
		bool bAnchored = false;
		bool bPillar = false;
		/** Mobilier / postes : reçoivent de l'intégrité mais ne soutiennent jamais d'autre pièce. */
		bool bLeaf = false;
	};

	/** Pièce enregistrée sans support, à revérifier quand son délai de grâce est écoulé. */
	struct FPendingValidation
	{
		TWeakObjectPtr<AEMBBuildablePiece> Piece;
		double ReadyTime = 0.0;
	};

	bool IsAuthority() const;

	/** Nouveau marqueur de visite ; gère le débordement du compteur (remise à zéro de tous les marqueurs). */
	uint32 NextStamp() const;

	int32 AllocateNode();
	void FreeNode(int32 NodeId);
	void InsertIntoCells(int32 NodeId);
	void RemoveFromCells(int32 NodeId);
	void FindContactNodes(const FBox& Bounds, int32 ExcludeNode, FNodeIdArray& OutNodes) const;

	/** Retire un lot de noeuds ; exécute éventuellement l'effondrement visuel ; recalcule le sous-graphe dépendant (serveur). */
	void RemoveBatch(const TArray<AEMBBuildablePiece*>& Batch, bool bExecuteCollapse, bool bRecompute);

	/** Relaxation incrémentale après ajout d'un noeud (serveur). */
	void PropagateAfterAdd(int32 NodeId);

	/**
	 * Énumère le sous-arbre dépendant des noeuds retirés (noeuds dont le chemin optimal passe par eux), avec leur
	 * profondeur. Retourne le marqueur de visite identifiant l'ensemble.
	 */
	uint32 CollectDependentNodes(const TArray<int32>& RemovedIds, TArray<int32>& OutAffected);

	/** Recalcule l'intégrité du sous-graphe dépendant et met en file les pièces devenues instables. */
	void RecomputeAffected(const TArray<int32>& Affected, uint32 AffectedStamp, const FVector& Origin);

	void WriteBackIntegrity(int32 NodeId);
	void EnqueueUnstable(const TArray<int32>& UnstableNodes, const FVector& Origin);
	void EnsureCollapseTimer();
	void ProcessCollapseWave();
	void FlushDirectCollapses();
	void ValidatePendingPieces();

private:
	TArray<FNode> Nodes;
	TArray<int32> FreeNodes;
	TMap<const AEMBBuildablePiece*, int32> PieceToNode;

	/** Index spatial : cellule -> noeuds dont la boîte la recouvre. */
	TMap<FIntVector, TArray<int32>> Cells;

	mutable uint32 CurrentStamp = 0;

	/** Vrai entre BeginBulkLoad et EndBulkLoad. */
	bool bBulkLoading = false;

	/** File FIFO des pièces à faire s'effondrer, triée par niveau (profondeur) à chaque insertion. */
	TArray<TWeakObjectPtr<AEMBBuildablePiece>> CollapseQueue;
	TSet<TWeakObjectPtr<AEMBBuildablePiece>> QueuedSet;

	/** Destructions directes (dégâts) en attente de regroupement en fin de frame. */
	TArray<TWeakObjectPtr<AEMBBuildablePiece>> DirectCollapseRequests;

	/** Pièces enregistrées sans support : revérifiées après leur délai de grâce. */
	TArray<FPendingValidation> PendingValidation;

	FTimerHandle CollapseTimerHandle;
	FTimerHandle ValidationTimerHandle;
	FTimerHandle DirectCollapseTimerHandle;
};
