// Emberfall - graphe de structure, intégrité et effondrement (Dev B).

#include "EMBStructureSubsystem.h"

#include "Emberfall.h"
#include "EMBBuildablePiece.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
	/** Taille d'une cellule de l'index spatial (cm). Une fondation fait 300 cm : ~4 pièces par cellule et par niveau. */
	constexpr float CellSize = 600.f;
	constexpr float InvCellSize = 1.f / CellSize;

	/** Tolérance de contact entre deux boîtes (cm) : absorbe les contacts face à face et les erreurs de flottants. */
	constexpr float ContactTolerance = 10.f;

	/** Nombre max de pièces par lot autorisées à produire des débris coûteux (Chaos / physique). */
	constexpr int32 MaxHeavyDebrisPerWave = 8;

	constexpr float IntegrityEpsilon = 0.001f;

	struct FHeapItem
	{
		float Integrity;
		int32 Node;
	};

	/** Prédicat de tas : la racine est l'élément d'intégrité MAXIMALE. */
	struct FIntegrityGreater
	{
		bool operator()(const FHeapItem& A, const FHeapItem& B) const
		{
			return A.Integrity > B.Integrity;
		}
	};

	struct FQueuedPiece
	{
		TWeakObjectPtr<AEMBBuildablePiece> Piece;
		int32 Depth = 0;
		double DistSq = 0.0;
	};

	FIntVector CellOf(const FVector& Point)
	{
		return FIntVector(
			FMath::FloorToInt32(Point.X * InvCellSize),
			FMath::FloorToInt32(Point.Y * InvCellSize),
			FMath::FloorToInt32(Point.Z * InvCellSize));
	}

	void ForEachCell(const FBox& Box, TFunctionRef<void(const FIntVector&)> Callback)
	{
		const FIntVector Min = CellOf(Box.Min);
		const FIntVector Max = CellOf(Box.Max);
		for (int32 X = Min.X; X <= Max.X; ++X)
		{
			for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
			{
				for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
				{
					Callback(FIntVector(X, Y, Z));
				}
			}
		}
	}

	/**
	 * Deux pièces sont en contact structurel si leurs boîtes se touchent PAR UNE FACE : tolérance de contact sur les
	 * trois axes et recouvrement réel (>= MinContactSize) sur au moins deux axes. Un contact par une arête ou un coin
	 * (fondations en diagonale) ne transmet pas la charge.
	 */
	bool AreInContact(const FBox& A, const FBox& B, float MinContactSize)
	{
		const double OverlapX = FMath::Min(A.Max.X, B.Max.X) - FMath::Max(A.Min.X, B.Min.X);
		const double OverlapY = FMath::Min(A.Max.Y, B.Max.Y) - FMath::Max(A.Min.Y, B.Min.Y);
		const double OverlapZ = FMath::Min(A.Max.Z, B.Max.Z) - FMath::Max(A.Min.Z, B.Min.Z);

		if (OverlapX < -ContactTolerance || OverlapY < -ContactTolerance || OverlapZ < -ContactTolerance)
		{
			return false;
		}

		const int32 OverlappingAxes = (OverlapX >= MinContactSize ? 1 : 0) + (OverlapY >= MinContactSize ? 1 : 0) + (OverlapZ >= MinContactSize ? 1 : 0);
		return OverlappingAxes >= 2;
	}

	/** Décroissance d'intégrité de la pièce (matériau de la pièce elle-même, pas celui du support). */
	float ComputeDecay(EEMBBuildMaterial Material, bool bPillar, bool bVertical)
	{
		const FEMBMaterialStats Stats = EMBBuildingDefaults::GetMaterialStats(Material);
		if (bVertical)
		{
			return Stats.VerticalDecay * (bPillar ? EMBBuildingDefaults::PillarVerticalDecayMultiplier : 1.f);
		}
		return Stats.HorizontalDecay;
	}

	bool IsVerticalLink(float PieceCenterZ, float SupportCenterZ)
	{
		return (PieceCenterZ - SupportCenterZ) > EMBBuildingDefaults::VerticalDeltaThreshold;
	}
}

UEMBStructureSubsystem* UEMBStructureSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UEMBStructureSubsystem>() : nullptr;
}

bool UEMBStructureSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Pas de graphe dans les mondes d'éditeur ou de prévisualisation.
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UEMBStructureSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(CollapseTimerHandle);
		TimerManager.ClearTimer(ValidationTimerHandle);
		TimerManager.ClearTimer(DirectCollapseTimerHandle);
	}

	Nodes.Reset();
	FreeNodes.Reset();
	PieceToNode.Reset();
	Cells.Reset();
	CollapseQueue.Reset();
	QueuedSet.Reset();
	DirectCollapseRequests.Reset();
	PendingValidation.Reset();

	Super::Deinitialize();
}

bool UEMBStructureSubsystem::IsAuthority() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}

uint32 UEMBStructureSubsystem::NextStamp() const
{
	// Si le compteur 32 bits déborde (après ~4 milliards de parcours), un ancien marqueur pourrait être pris pour
	// le courant : on remet tous les marqueurs à zéro et on repart de 1.
	if (++CurrentStamp == 0)
	{
		for (const FNode& Node : Nodes)
		{
			Node.VisitStamp = 0;
		}
		CurrentStamp = 1;
	}
	return CurrentStamp;
}

// ---------------------------------------------------------------------------------------------------------------------
// Noeuds et index spatial
// ---------------------------------------------------------------------------------------------------------------------

int32 UEMBStructureSubsystem::AllocateNode()
{
	int32 NodeId;
	if (FreeNodes.Num() > 0)
	{
		NodeId = FreeNodes.Pop();
		Nodes[NodeId] = FNode();
	}
	else
	{
		NodeId = Nodes.AddDefaulted();
	}
	Nodes[NodeId].bInUse = true;
	return NodeId;
}

void UEMBStructureSubsystem::FreeNode(int32 NodeId)
{
	Nodes[NodeId] = FNode(); // bInUse repasse à false, marqueur et parent remis à zéro
	FreeNodes.Add(NodeId);
}

void UEMBStructureSubsystem::InsertIntoCells(int32 NodeId)
{
	ForEachCell(Nodes[NodeId].Bounds, [this, NodeId](const FIntVector& Cell)
	{
		Cells.FindOrAdd(Cell).Add(NodeId);
	});
}

void UEMBStructureSubsystem::RemoveFromCells(int32 NodeId)
{
	ForEachCell(Nodes[NodeId].Bounds, [this, NodeId](const FIntVector& Cell)
	{
		if (TArray<int32>* CellNodes = Cells.Find(Cell))
		{
			CellNodes->RemoveSingleSwap(NodeId);
			if (CellNodes->IsEmpty())
			{
				Cells.Remove(Cell);
			}
		}
	});
}

void UEMBStructureSubsystem::FindContactNodes(const FBox& Bounds, int32 ExcludeNode, FNodeIdArray& OutNodes) const
{
	// Recherche limitée aux cellules recouvertes par la boîte élargie de la tolérance : coût indépendant de la taille du monde.
	const FBox Search = Bounds.ExpandBy(ContactTolerance);
	ForEachCell(Search, [this, &Bounds, ExcludeNode, &OutNodes](const FIntVector& Cell)
	{
		const TArray<int32>* CellNodes = Cells.Find(Cell);
		if (!CellNodes)
		{
			return;
		}
		for (const int32 NodeId : *CellNodes)
		{
			if (NodeId != ExcludeNode && !OutNodes.Contains(NodeId) && AreInContact(Bounds, Nodes[NodeId].Bounds, MinContactSize))
			{
				OutNodes.Add(NodeId);
			}
		}
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Enregistrement
// ---------------------------------------------------------------------------------------------------------------------

void UEMBStructureSubsystem::RegisterPiece(AEMBBuildablePiece* Piece)
{
	if (!IsValid(Piece) || PieceToNode.Contains(Piece))
	{
		return;
	}

	const int32 NodeId = AllocateNode();
	{
		FNode& Node = Nodes[NodeId];
		Node.Piece = Piece;
		Node.Bounds = Piece->GetStructureBounds();
		Node.CenterZ = static_cast<float>(Node.Bounds.GetCenter().Z);
		Node.Material = Piece->GetMaterial();
		Node.bAnchored = Piece->IsAnchoredToGround();
		Node.bPillar = Piece->GetCategory() == EEMBPieceCategory::Pillar;
		Node.bLeaf = Piece->GetCategory() == EEMBPieceCategory::Furniture || Piece->GetCategory() == EEMBPieceCategory::Workstation;
	}
	PieceToNode.Add(Piece, NodeId);

	// Connexions de support : déterminées une fois pour toutes au placement, par contact des boîtes.
	FNodeIdArray Contacts;
	FindContactNodes(Nodes[NodeId].Bounds, NodeId, Contacts);
	for (const int32 ContactId : Contacts)
	{
		Nodes[NodeId].Neighbors.Add(ContactId);
		Nodes[ContactId].Neighbors.Add(NodeId);
	}
	InsertIntoCells(NodeId);

	// Pendant un chargement en masse, l'intégrité est calculée une seule fois à la fin (EndBulkLoad).
	if (IsAuthority() && !bBulkLoading)
	{
		PropagateAfterAdd(NodeId);
	}
}

void UEMBStructureSubsystem::BeginBulkLoad()
{
	if (IsAuthority())
	{
		bBulkLoading = true;
	}
}

int32 UEMBStructureSubsystem::EndBulkLoad()
{
	if (!bBulkLoading)
	{
		return 0;
	}
	bBulkLoading = false;
	if (!IsAuthority())
	{
		return 0;
	}

	// Recalcul global : tous les noeuds en service forment l'ensemble « affecté ». RecomputeAffected amorce les
	// ancres à 100, résout par Dijkstra, écrit les intégrités et met en file les pièces non soutenues.
	TArray<int32> All;
	All.Reserve(PieceToNode.Num());
	const uint32 Stamp = NextStamp();
	for (int32 NodeId = 0; NodeId < Nodes.Num(); ++NodeId)
	{
		if (Nodes[NodeId].bInUse)
		{
			Nodes[NodeId].VisitStamp = Stamp;
			Nodes[NodeId].Depth = 0;
			All.Add(NodeId);
		}
	}
	RecomputeAffected(All, Stamp, FVector::ZeroVector);

	// RecomputeAffected a mis en file les pièces non soutenues : on les détruit immédiatement et silencieusement.
	// Une pièce non soutenue ne soutient personne, donc leur retrait ne change l'intégrité d'aucune autre pièce
	// (aucun recalcul nécessaire), exactement comme pour les vagues de ProcessCollapseWave.
	TArray<AEMBBuildablePiece*> Unsupported;
	for (const TWeakObjectPtr<AEMBBuildablePiece>& Weak : CollapseQueue)
	{
		if (AEMBBuildablePiece* Piece = Weak.Get())
		{
			Unsupported.Add(Piece);
		}
	}
	CollapseQueue.Reset();
	QueuedSet.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CollapseTimerHandle);
	}

	RemoveBatch(Unsupported, /*bExecuteCollapse*/ false, /*bRecompute*/ false);
	for (AEMBBuildablePiece* Piece : Unsupported)
	{
		Piece->Destroy();
	}
	return Unsupported.Num();
}

void UEMBStructureSubsystem::GetAllPieces(TArray<AEMBBuildablePiece*>& OutPieces) const
{
	OutPieces.Reserve(OutPieces.Num() + PieceToNode.Num());
	for (const FNode& Node : Nodes)
	{
		if (Node.bInUse)
		{
			if (AEMBBuildablePiece* Piece = Node.Piece.Get())
			{
				OutPieces.Add(Piece);
			}
		}
	}
}

void UEMBStructureSubsystem::UnregisterPiece(AEMBBuildablePiece* Piece, bool bRecomputeNeighbors)
{
	if (!Piece || !PieceToNode.Contains(Piece))
	{
		return;
	}

	TArray<AEMBBuildablePiece*> Single;
	Single.Add(Piece);
	RemoveBatch(Single, /*bExecuteCollapse*/ false, bRecomputeNeighbors);
}

void UEMBStructureSubsystem::RequestCollapse(AEMBBuildablePiece* Piece)
{
	if (!IsAuthority() || !Piece)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		Piece->ExecuteCollapse(true);
		return;
	}

	DirectCollapseRequests.AddUnique(Piece);
	if (DirectCollapseRequests.Num() == 1)
	{
		// Un seul rendez-vous par frame : toutes les destructions directes arrivées avant sont traitées ensemble.
		DirectCollapseTimerHandle = World->GetTimerManager().SetTimerForNextTick(this, &UEMBStructureSubsystem::FlushDirectCollapses);
	}
}

void UEMBStructureSubsystem::FlushDirectCollapses()
{
	TArray<AEMBBuildablePiece*> Batch;
	for (const TWeakObjectPtr<AEMBBuildablePiece>& Weak : DirectCollapseRequests)
	{
		AEMBBuildablePiece* Piece = Weak.Get();
		if (Piece && !Piece->IsCollapsed())
		{
			Batch.Add(Piece);
		}
	}
	DirectCollapseRequests.Reset();

	if (Batch.Num() > 0)
	{
		// UN retrait de lot et UN recalcul du sous-graphe dépendant pour toutes les pièces détruites cette frame.
		RemoveBatch(Batch, /*bExecuteCollapse*/ true, /*bRecompute*/ true);
	}
}

void UEMBStructureSubsystem::CollapsePiecesNow(const TArray<AEMBBuildablePiece*>& Pieces)
{
	if (!IsAuthority())
	{
		return;
	}
	RemoveBatch(Pieces, /*bExecuteCollapse*/ true, /*bRecompute*/ true);
}

void UEMBStructureSubsystem::RemoveBatch(const TArray<AEMBBuildablePiece*>& Batch, bool bExecuteCollapse, bool bRecompute)
{
	// 1) Noeuds du lot et point de rupture (barycentre) servant à ordonner la cascade.
	TArray<int32> BatchIds;
	for (const AEMBBuildablePiece* Piece : Batch)
	{
		if (const int32* NodeId = PieceToNode.Find(Piece))
		{
			BatchIds.AddUnique(*NodeId);
		}
	}

	FVector Origin = FVector::ZeroVector;
	for (const int32 NodeId : BatchIds)
	{
		Origin += Nodes[NodeId].Bounds.GetCenter();
	}
	if (BatchIds.Num() > 0)
	{
		Origin /= static_cast<double>(BatchIds.Num());
	}
	else if (Batch.Num() > 0 && Batch[0])
	{
		Origin = Batch[0]->GetActorLocation();
	}

	// 2) Sous-graphe dépendant, à énumérer AVANT le détachement (il repose sur les pointeurs de parent).
	TArray<int32> Affected;
	uint32 AffectedStamp = 0;
	const bool bDoRecompute = bRecompute && IsAuthority() && BatchIds.Num() > 0;
	if (bDoRecompute)
	{
		AffectedStamp = CollectDependentNodes(BatchIds, Affected);
	}

	// 3) Détacher puis libérer les noeuds.
	for (const int32 NodeId : BatchIds)
	{
		RemoveFromCells(NodeId);
		for (const int32 NeighborId : Nodes[NodeId].Neighbors)
		{
			Nodes[NeighborId].Neighbors.RemoveSingleSwap(NodeId);
		}
	}
	for (const AEMBBuildablePiece* Piece : Batch)
	{
		PieceToNode.Remove(Piece);
	}
	for (const int32 NodeId : BatchIds)
	{
		FreeNode(NodeId);
	}

	// 4) Effondrement visuel. Seules les premières pièces du lot peuvent produire des débris lourds (quota).
	if (bExecuteCollapse)
	{
		int32 Index = 0;
		for (AEMBBuildablePiece* Piece : Batch)
		{
			if (IsValid(Piece))
			{
				Piece->ExecuteCollapse(Index < MaxHeavyDebrisPerWave);
			}
			++Index;
		}
	}

	// 5) Recalcul du seul sous-graphe dépendant.
	if (bDoRecompute && Affected.Num() > 0)
	{
		RecomputeAffected(Affected, AffectedStamp, Origin);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Intégrité
// ---------------------------------------------------------------------------------------------------------------------

void UEMBStructureSubsystem::WriteBackIntegrity(int32 NodeId)
{
	// SetIntegrityServer ignore les valeurs inchangées : pas de réveil de dormance inutile.
	if (AEMBBuildablePiece* Piece = Nodes[NodeId].Piece.Get())
	{
		Piece->SetIntegrityServer(Nodes[NodeId].Integrity);
	}
}

void UEMBStructureSubsystem::PropagateAfterAdd(int32 NodeId)
{
	// Intégrité de la nouvelle pièce : max sur ses supports, avec mémorisation du support parent.
	float Start = 0.f;
	int32 BestParent = INDEX_NONE;
	{
		const FNode& NewNode = Nodes[NodeId];
		if (NewNode.bAnchored)
		{
			Start = EMBBuildingDefaults::GroundedIntegrity;
		}
		else
		{
			for (const int32 NeighborId : NewNode.Neighbors)
			{
				const FNode& Support = Nodes[NeighborId];
				if (Support.bLeaf || Support.Integrity <= IntegrityEpsilon)
				{
					continue;
				}
				const bool bVertical = IsVerticalLink(NewNode.CenterZ, Support.CenterZ);
				const float Candidate = Support.Integrity - ComputeDecay(NewNode.Material, NewNode.bPillar, bVertical);
				if (Candidate > Start)
				{
					Start = Candidate;
					BestParent = NeighborId;
				}
			}
		}
	}

	Nodes[NodeId].Integrity = FMath::Max(Start, 0.f);
	Nodes[NodeId].Parent = BestParent;
	WriteBackIntegrity(NodeId);

	if (Nodes[NodeId].Integrity <= IntegrityEpsilon)
	{
		// Pièce non soutenue au moment de son enregistrement. Pièce pré-placée / streamée : l'ordre de BeginPlay est
		// arbitraire (un mur peut s'enregistrer avant sa fondation, ou avant le chargement d'une cellule voisine),
		// on laisse donc PreplacedGracePeriod (1,0 s) avant la première évaluation. Pièce posée par un joueur : le
		// serveur a déjà validé le placement, on ne revérifie qu'à la frame suivante.
		UWorld* World = GetWorld();
		const AEMBBuildablePiece* Piece = Nodes[NodeId].Piece.Get();
		if (World && Piece)
		{
			const double Delay = Piece->WasPlacedByPlayer() ? 0.0 : static_cast<double>(EMBBuildingDefaults::PreplacedGracePeriod);
			FPendingValidation& Pending = PendingValidation.AddDefaulted_GetRef();
			Pending.Piece = Nodes[NodeId].Piece;
			Pending.ReadyTime = World->GetTimeSeconds() + Delay;

			FTimerManager& TimerManager = World->GetTimerManager();
			if (!TimerManager.IsTimerActive(ValidationTimerHandle))
			{
				TimerManager.SetTimer(ValidationTimerHandle, this, &UEMBStructureSubsystem::ValidatePendingPieces, FMath::Max(static_cast<float>(Delay), 0.01f), /*bLoop*/ false);
			}
		}
		return;
	}

	if (Nodes[NodeId].bLeaf)
	{
		return;
	}

	// Relaxation incrémentale : ajouter un noeud ne peut qu'AMÉLIORER des intégrités. On ne revisite donc que les
	// pièces dont la valeur augmente ; les autres ne sont jamais touchées.
	TArray<FHeapItem> Heap;
	Heap.HeapPush(FHeapItem{ Nodes[NodeId].Integrity, NodeId }, FIntegrityGreater());

	while (Heap.Num() > 0)
	{
		FHeapItem Top;
		Heap.HeapPop(Top, FIntegrityGreater());

		const FNode& Current = Nodes[Top.Node];
		if (Top.Integrity < Current.Integrity || Current.bLeaf)
		{
			continue; // entrée périmée ou noeud non porteur
		}

		for (const int32 NeighborId : Current.Neighbors)
		{
			FNode& Neighbor = Nodes[NeighborId];
			if (Neighbor.bAnchored)
			{
				continue;
			}
			const bool bVertical = IsVerticalLink(Neighbor.CenterZ, Current.CenterZ);
			const float Candidate = Current.Integrity - ComputeDecay(Neighbor.Material, Neighbor.bPillar, bVertical);
			if (Candidate > Neighbor.Integrity + IntegrityEpsilon)
			{
				Neighbor.Integrity = Candidate;
				Neighbor.Parent = Top.Node;
				WriteBackIntegrity(NeighborId);
				Heap.HeapPush(FHeapItem{ Candidate, NeighborId }, FIntegrityGreater());
			}
		}
	}
}

uint32 UEMBStructureSubsystem::CollectDependentNodes(const TArray<int32>& RemovedIds, TArray<int32>& OutAffected)
{
	// Sous-arbre dépendant : un noeud est affecté si son support parent est un noeud retiré, ou un noeud affecté.
	// Les autres noeuds ont un chemin optimal intact (retirer des noeuds ne peut pas améliorer un chemin) et
	// conservent leur intégrité telle quelle.
	const uint32 Stamp = NextStamp();
	for (const int32 RemovedId : RemovedIds)
	{
		Nodes[RemovedId].VisitStamp = Stamp; // les noeuds retirés ne font pas partie de l'ensemble affecté
	}

	// Parcours en largeur : la profondeur (niveau) sert ensuite à ordonner la cascade.
	for (const int32 RemovedId : RemovedIds)
	{
		for (const int32 NeighborId : Nodes[RemovedId].Neighbors)
		{
			FNode& Neighbor = Nodes[NeighborId];
			if (Neighbor.VisitStamp != Stamp && RemovedIds.Contains(Neighbor.Parent))
			{
				Neighbor.VisitStamp = Stamp;
				Neighbor.Depth = 0;
				OutAffected.Add(NeighborId);
			}
		}
	}

	for (int32 Head = 0; Head < OutAffected.Num(); ++Head)
	{
		const int32 CurrentId = OutAffected[Head];
		const int32 CurrentDepth = Nodes[CurrentId].Depth;
		for (const int32 NeighborId : Nodes[CurrentId].Neighbors)
		{
			FNode& Neighbor = Nodes[NeighborId];
			if (Neighbor.VisitStamp != Stamp && Neighbor.Parent == CurrentId)
			{
				Neighbor.VisitStamp = Stamp;
				Neighbor.Depth = CurrentDepth + 1;
				OutAffected.Add(NeighborId);
			}
		}
	}
	return Stamp;
}

void UEMBStructureSubsystem::RecomputeAffected(const TArray<int32>& Affected, uint32 AffectedStamp, const FVector& Origin)
{
	// Remise à zéro de l'ensemble affecté.
	for (const int32 NodeId : Affected)
	{
		Nodes[NodeId].Integrity = 0.f;
		Nodes[NodeId].Parent = INDEX_NONE;
	}

	// Amorçage : meilleure valeur obtenue depuis un voisin NON affecté (intégrité fiable et inchangée).
	TArray<FHeapItem> Heap;
	Heap.Reserve(Affected.Num());
	for (const int32 NodeId : Affected)
	{
		FNode& Node = Nodes[NodeId];

		// Ancre au sol : intégrité fixe. Ne se produit jamais dans un retrait ordinaire (une ancre n'a pas de
		// parent), mais c'est le point de départ du recalcul global d'un chargement de sauvegarde.
		if (Node.bAnchored)
		{
			Node.Integrity = EMBBuildingDefaults::GroundedIntegrity;
			Node.Parent = INDEX_NONE;
			Heap.HeapPush(FHeapItem{ Node.Integrity, NodeId }, FIntegrityGreater());
			continue;
		}

		float Best = 0.f;
		int32 BestParent = INDEX_NONE;
		for (const int32 NeighborId : Node.Neighbors)
		{
			const FNode& Support = Nodes[NeighborId];
			if (Support.VisitStamp == AffectedStamp || Support.bLeaf || Support.Integrity <= IntegrityEpsilon)
			{
				continue;
			}
			const bool bVertical = IsVerticalLink(Node.CenterZ, Support.CenterZ);
			const float Candidate = Support.Integrity - ComputeDecay(Node.Material, Node.bPillar, bVertical);
			if (Candidate > Best)
			{
				Best = Candidate;
				BestParent = NeighborId;
			}
		}
		if (Best > IntegrityEpsilon)
		{
			Node.Integrity = Best;
			Node.Parent = BestParent;
			Heap.HeapPush(FHeapItem{ Best, NodeId }, FIntegrityGreater());
		}
	}

	// Dijkstra local, restreint à l'ensemble affecté : O(s log s).
	while (Heap.Num() > 0)
	{
		FHeapItem Top;
		Heap.HeapPop(Top, FIntegrityGreater());

		const FNode& Current = Nodes[Top.Node];
		if (Top.Integrity < Current.Integrity || Current.bLeaf)
		{
			continue;
		}

		for (const int32 NeighborId : Current.Neighbors)
		{
			FNode& Neighbor = Nodes[NeighborId];
			if (Neighbor.VisitStamp != AffectedStamp)
			{
				continue;
			}
			const bool bVertical = IsVerticalLink(Neighbor.CenterZ, Current.CenterZ);
			const float Candidate = Current.Integrity - ComputeDecay(Neighbor.Material, Neighbor.bPillar, bVertical);
			// Un candidat <= 0 n'est jamais retenu : le noeud reste à 0 (non soutenu) et ne propage rien.
			if (Candidate > Neighbor.Integrity + IntegrityEpsilon)
			{
				Neighbor.Integrity = Candidate;
				Neighbor.Parent = Top.Node;
				Heap.HeapPush(FHeapItem{ Candidate, NeighborId }, FIntegrityGreater());
			}
		}
	}

	TArray<int32> Unstable;
	for (const int32 NodeId : Affected)
	{
		WriteBackIntegrity(NodeId);
		const AEMBBuildablePiece* Piece = Nodes[NodeId].Piece.Get();
		if (Nodes[NodeId].Integrity <= IntegrityEpsilon && Piece && !Piece->IsCollapsed())
		{
			Unstable.Add(NodeId);
		}
	}
	EnqueueUnstable(Unstable, Origin);
}

float UEMBStructureSubsystem::PredictIntegrity(const FEMBStructureProbe& Probe, int32* OutSupportCount) const
{
	FNodeIdArray Contacts;
	FindContactNodes(Probe.WorldBounds, INDEX_NONE, Contacts);

	const float ProbeCenterZ = static_cast<float>(Probe.WorldBounds.GetCenter().Z);
	float Best = 0.f;
	int32 SupportCount = 0;

	for (const int32 NodeId : Contacts)
	{
		const FNode& Support = Nodes[NodeId];
		const AEMBBuildablePiece* SupportPiece = Support.Piece.Get();
		if (Support.bLeaf || !SupportPiece || SupportPiece->IsCollapsed())
		{
			continue;
		}
		++SupportCount;

		// On lit l'intégrité RÉPLIQUÉE de la pièce (valable sur client comme sur serveur).
		const bool bVertical = IsVerticalLink(ProbeCenterZ, Support.CenterZ);
		Best = FMath::Max(Best, SupportPiece->GetIntegrity() - ComputeDecay(Probe.Material, Probe.bIsPillar, bVertical));
	}

	if (OutSupportCount)
	{
		*OutSupportCount = SupportCount;
	}
	return Best;
}

int32 UEMBStructureSubsystem::CountPiecesInTouchedBases(const FEMBStructureProbe& Probe, int32 Limit) const
{
	FNodeIdArray Contacts;
	FindContactNodes(Probe.WorldBounds, INDEX_NONE, Contacts);
	if (Contacts.Num() == 0)
	{
		return 0;
	}

	// Parcours en largeur interrompu dès que la limite est dépassée : coût borné par Limit, quelle que soit la taille réelle.
	const uint32 Stamp = NextStamp();
	TArray<int32> Stack;
	for (const int32 ContactId : Contacts)
	{
		if (Nodes[ContactId].VisitStamp != Stamp)
		{
			Nodes[ContactId].VisitStamp = Stamp;
			Stack.Add(ContactId);
		}
	}

	int32 Count = 0;
	while (Stack.Num() > 0 && Count <= Limit)
	{
		const int32 NodeId = Stack.Pop();
		++Count;
		for (const int32 NeighborId : Nodes[NodeId].Neighbors)
		{
			if (Nodes[NeighborId].VisitStamp != Stamp)
			{
				Nodes[NeighborId].VisitStamp = Stamp;
				Stack.Add(NeighborId);
			}
		}
	}
	return Count;
}

int32 UEMBStructureSubsystem::GetBaseId(const AEMBBuildablePiece* Piece) const
{
	const int32* StartId = PieceToNode.Find(Piece);
	if (!StartId)
	{
		return INDEX_NONE;
	}

	// Composante connexe de la pièce ; l'identifiant est le hachage de l'angle minimal (X, puis Y, puis Z) de la composante :
	// il ne dépend ni de l'ordre d'enregistrement ni de la réutilisation des identifiants de noeuds.
	const uint32 Stamp = NextStamp();
	TArray<int32> Stack;
	Stack.Add(*StartId);
	Nodes[*StartId].VisitStamp = Stamp;

	FIntVector MinCorner(MAX_int32, MAX_int32, MAX_int32);
	while (Stack.Num() > 0)
	{
		const int32 NodeId = Stack.Pop();
		const FVector Corner = Nodes[NodeId].Bounds.Min;
		const FIntVector Rounded(FMath::RoundToInt32(Corner.X), FMath::RoundToInt32(Corner.Y), FMath::RoundToInt32(Corner.Z));
		if (Rounded.X < MinCorner.X
			|| (Rounded.X == MinCorner.X && Rounded.Y < MinCorner.Y)
			|| (Rounded.X == MinCorner.X && Rounded.Y == MinCorner.Y && Rounded.Z < MinCorner.Z))
		{
			MinCorner = Rounded;
		}
		for (const int32 NeighborId : Nodes[NodeId].Neighbors)
		{
			if (Nodes[NeighborId].VisitStamp != Stamp)
			{
				Nodes[NeighborId].VisitStamp = Stamp;
				Stack.Add(NeighborId);
			}
		}
	}
	return static_cast<int32>(GetTypeHash(MinCorner));
}

// ---------------------------------------------------------------------------------------------------------------------
// Requêtes spatiales
// ---------------------------------------------------------------------------------------------------------------------

void UEMBStructureSubsystem::QueryPiecesInBox(const FBox& Box, TArray<AEMBBuildablePiece*>& OutPieces) const
{
	TArray<int32, TInlineAllocator<32>> Seen;
	ForEachCell(Box, [this, &Box, &Seen, &OutPieces](const FIntVector& Cell)
	{
		const TArray<int32>* CellNodes = Cells.Find(Cell);
		if (!CellNodes)
		{
			return;
		}
		for (const int32 NodeId : *CellNodes)
		{
			if (!Seen.Contains(NodeId))
			{
				Seen.Add(NodeId);
				if (Nodes[NodeId].Bounds.Intersect(Box))
				{
					if (AEMBBuildablePiece* Piece = Nodes[NodeId].Piece.Get())
					{
						OutPieces.Add(Piece);
					}
				}
			}
		}
	});
}

void UEMBStructureSubsystem::QueryPiecesInSphere(const FVector& Center, float Radius, TArray<AEMBBuildablePiece*>& OutPieces) const
{
	const FVector Extent(Radius);
	const FBox Box(Center - Extent, Center + Extent);
	const double RadiusSq = FMath::Square(static_cast<double>(Radius));

	TArray<int32, TInlineAllocator<32>> Seen;
	ForEachCell(Box, [this, &Center, RadiusSq, &Seen, &OutPieces](const FIntVector& Cell)
	{
		const TArray<int32>* CellNodes = Cells.Find(Cell);
		if (!CellNodes)
		{
			return;
		}
		for (const int32 NodeId : *CellNodes)
		{
			if (!Seen.Contains(NodeId))
			{
				Seen.Add(NodeId);
				if (Nodes[NodeId].Bounds.ComputeSquaredDistanceToPoint(Center) <= RadiusSq)
				{
					if (AEMBBuildablePiece* Piece = Nodes[NodeId].Piece.Get())
					{
						OutPieces.Add(Piece);
					}
				}
			}
		}
	});
}

void UEMBStructureSubsystem::GetNeighbors(const AEMBBuildablePiece* Piece, TArray<AEMBBuildablePiece*>& OutNeighbors) const
{
	const int32* NodeId = PieceToNode.Find(Piece);
	if (!NodeId)
	{
		return;
	}
	for (const int32 NeighborId : Nodes[*NodeId].Neighbors)
	{
		if (AEMBBuildablePiece* Neighbor = Nodes[NeighborId].Piece.Get())
		{
			OutNeighbors.Add(Neighbor);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Effondrement en cascade
// ---------------------------------------------------------------------------------------------------------------------

void UEMBStructureSubsystem::EnqueueUnstable(const TArray<int32>& UnstableNodes, const FVector& Origin)
{
	if (UnstableNodes.Num() == 0)
	{
		return;
	}

	TArray<FQueuedPiece> ToAdd;
	for (const int32 NodeId : UnstableNodes)
	{
		const TWeakObjectPtr<AEMBBuildablePiece>& Piece = Nodes[NodeId].Piece;
		if (!Piece.IsValid() || QueuedSet.Contains(Piece))
		{
			continue;
		}
		FQueuedPiece Entry;
		Entry.Piece = Piece;
		Entry.Depth = Nodes[NodeId].Depth;
		Entry.DistSq = FVector::DistSquared(Nodes[NodeId].Bounds.GetCenter(), Origin);
		ToAdd.Add(Entry);
	}

	// Cascade PAR NIVEAUX : profondeur BFS depuis la cassure d'abord (la plus petite en premier), distance ensuite.
	ToAdd.Sort([](const FQueuedPiece& A, const FQueuedPiece& B)
	{
		return A.Depth != B.Depth ? A.Depth < B.Depth : A.DistSq < B.DistSq;
	});

	for (const FQueuedPiece& Entry : ToAdd)
	{
		CollapseQueue.Add(Entry.Piece);
		QueuedSet.Add(Entry.Piece);
	}

	if (ToAdd.Num() > 0)
	{
		EnsureCollapseTimer();
	}
}

void UEMBStructureSubsystem::EnsureCollapseTimer()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FTimerManager& TimerManager = World->GetTimerManager();
	if (!TimerManager.IsTimerActive(CollapseTimerHandle))
	{
		// Timer en boucle plutôt que Tick : le subsystem ne coûte rien tant qu'aucune structure ne s'effondre.
		TimerManager.SetTimer(CollapseTimerHandle, this, &UEMBStructureSubsystem::ProcessCollapseWave,
			EMBBuildingDefaults::CollapseWaveDelay, /*bLoop*/ true, EMBBuildingDefaults::CollapseWaveDelay);
	}
}

void UEMBStructureSubsystem::ProcessCollapseWave()
{
	// Une vague = au plus MaxCollapsePerFrame pièces, prises en tête de file (donc les niveaux les plus proches de la
	// cassure d'abord), 0,08 s après la précédente : cascade étalée dans le temps, sans pic de CPU ni de réplication.
	TArray<AEMBBuildablePiece*> Batch;
	int32 Consumed = 0;

	while (Consumed < CollapseQueue.Num() && Batch.Num() < EMBBuildingDefaults::MaxCollapsePerFrame)
	{
		const TWeakObjectPtr<AEMBBuildablePiece> Weak = CollapseQueue[Consumed++];
		QueuedSet.Remove(Weak);

		AEMBBuildablePiece* Piece = Weak.Get();
		if (!Piece || Piece->IsCollapsed())
		{
			continue;
		}
		const int32* NodeId = PieceToNode.Find(Piece);
		if (!NodeId)
		{
			continue; // déjà retirée du graphe (détruite par dégâts entre-temps)
		}
		if (Nodes[*NodeId].Integrity > EMBBuildingDefaults::CollapseThreshold + IntegrityEpsilon)
		{
			continue; // sauvée par un nouveau support posé pendant la cascade
		}
		Batch.Add(Piece);
	}

	CollapseQueue.RemoveAt(0, Consumed);

	if (Batch.Num() > 0)
	{
		// AUCUN recalcul ici (bRecompute = false) : le recalcul initial a déjà mis en file TOUTES les pièces privées de
		// support. Une pièce non soutenue (intégrité 0) ne soutient personne, donc la retirer ne change l'intégrité
		// d'aucune autre pièce : un nouveau calcul à chaque vague serait du travail pur perdu.
		RemoveBatch(Batch, /*bExecuteCollapse*/ true, /*bRecompute*/ false);
	}

	if (CollapseQueue.Num() == 0)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(CollapseTimerHandle);
		}
	}
}

void UEMBStructureSubsystem::ValidatePendingPieces()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	TArray<int32> Unstable;
	TArray<FPendingValidation> Remaining;
	FVector Origin = FVector::ZeroVector;
	double NextReady = TNumericLimits<double>::Max();

	for (const FPendingValidation& Pending : PendingValidation)
	{
		const AEMBBuildablePiece* Piece = Pending.Piece.Get();
		if (!Piece)
		{
			continue;
		}
		if (Pending.ReadyTime > Now)
		{
			Remaining.Add(Pending);
			NextReady = FMath::Min(NextReady, Pending.ReadyTime);
			continue;
		}
		const int32* NodeId = PieceToNode.Find(Piece);
		if (NodeId && Nodes[*NodeId].Integrity <= IntegrityEpsilon)
		{
			Nodes[*NodeId].Depth = 0;
			Unstable.Add(*NodeId);
			Origin += Nodes[*NodeId].Bounds.GetCenter();
		}
	}
	PendingValidation = MoveTemp(Remaining);

	if (Unstable.Num() > 0)
	{
		Origin /= static_cast<double>(Unstable.Num());
		EnqueueUnstable(Unstable, Origin);
	}

	if (PendingValidation.Num() > 0)
	{
		World->GetTimerManager().SetTimer(ValidationTimerHandle, this, &UEMBStructureSubsystem::ValidatePendingPieces,
			FMath::Max(static_cast<float>(NextReady - Now), 0.01f), /*bLoop*/ false);
	}
}
