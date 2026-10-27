// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBResourceNodeSubsystem.h"

#include "Emberfall.h"
#include "EMBFallingTree.h"
#include "EMBResourceNode.h"
#include "EMBStructureSubsystem.h"

#include "Engine/AssetManager.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace
{
	/** Chemin par défaut de la table (surchargeable dans DefaultGame.ini). */
	const TCHAR* const DefaultNodesTablePath = TEXT("/Game/Data/DT_ResourceNodes.DT_ResourceNodes");
}

UEMBResourceNodeSubsystem::UEMBResourceNodeSubsystem()
{
	NodesTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(DefaultNodesTablePath));
}

bool UEMBResourceNodeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	// Mondes de jeu seulement (PIE, jeu packagé, serveur) : pas d'aperçus d'éditeur ni de mondes d'asset.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UEMBResourceNodeSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RespawnTimerHandle);
	}
	PlacedNodes.Reset();
	Respawns.Reset();
	DynamicNodes.Reset();
	FallingTrees.Reset();
	LoadedTable = nullptr;
	NodeClassHandles.Reset();
	bTableLoadAttempted = false;
	Super::Deinitialize();
}

UEMBResourceNodeSubsystem* UEMBResourceNodeSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UEMBResourceNodeSubsystem>() : nullptr;
}

// ----------------------------------------------------------------------------------------------
// Table
// ----------------------------------------------------------------------------------------------

void UEMBResourceNodeSubsystem::EnsureTableLoaded()
{
	if (bTableLoadAttempted)
	{
		return;
	}
	bTableLoadAttempted = true;

	// Chargement synchrone, une seule fois : la table compte une quinzaine de lignes et doit être prête avant le premier coup.
	UDataTable* Table = NodesTable.LoadSynchronous();
	if (!Table)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : DataTable des nœuds introuvable (%s)."), *NodesTable.ToString());
		return;
	}
	if (Table->GetRowStruct() != FEMBResourceNodeRow::StaticStruct())
	{
		UE_LOG(LogEmberfall, Error, TEXT("Récolte : %s n'utilise pas la structure FEMBResourceNodeRow, table ignorée."), *GetNameSafe(Table));
		return;
	}
	LoadedTable = Table;

	// Préchargement asynchrone des classes de carcasses : SpawnDynamicNode (mort d'une créature, en plein combat) ne
	// doit jamais déclencher un chargement synchrone.
	Table->ForeachRow<FEMBResourceNodeRow>(TEXT("EMB PreloadNodeClasses"), [this](const FName& /*Key*/, const FEMBResourceNodeRow& Row)
	{
		if (!Row.NodeClass.IsNull())
		{
			if (TSharedPtr<FStreamableHandle> Handle = UAssetManager::GetStreamableManager().RequestAsyncLoad(Row.NodeClass.ToSoftObjectPath()))
			{
				NodeClassHandles.Add(Handle);
			}
		}
	});
}

const FEMBResourceNodeRow* UEMBResourceNodeSubsystem::FindRow(const FName RowName) const
{
	return (LoadedTable && !RowName.IsNone()) ? LoadedTable->FindRow<FEMBResourceNodeRow>(RowName, TEXT("EMB FindResourceNodeRow"), false) : nullptr;
}

// ----------------------------------------------------------------------------------------------
// Registre des nœuds et réapparitions
// ----------------------------------------------------------------------------------------------

void UEMBResourceNodeSubsystem::RegisterNode(AEMBResourceNode* Node)
{
	if (!Node || !Node->HasAuthority())
	{
		return;
	}
	EnsureTableLoaded();
	if (Node->IsDynamicNode())
	{
		return; // les carcasses ne sont ni sauvegardées ni indexées
	}

	const FGuid Id = Node->GetNodeId();
	if (!Id.IsValid())
	{
		return;
	}
	if (const TWeakObjectPtr<AEMBResourceNode>* Existing = PlacedNodes.Find(Id))
	{
		if (Existing->IsValid() && Existing->Get() != Node)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Récolte : NodeId en double (%s et %s). Un des deux nœuds ne sera pas restauré correctement."),
				*GetNameSafe(Existing->Get()), *GetNameSafe(Node));
		}
	}
	PlacedNodes.Add(Id, Node);

	// Nœud chargé (ou rechargé par le streaming) alors qu'il est encore épuisé : il reprend son état.
	if (FRespawnEntry* Entry = Respawns.Find(Id))
	{
		Entry->Node = Node;
		Node->RestoreDepleted();
	}
}

void UEMBResourceNodeSubsystem::UnregisterNode(AEMBResourceNode* Node)
{
	if (!Node || Node->IsDynamicNode())
	{
		return;
	}
	const FGuid Id = Node->GetNodeId();
	if (const TWeakObjectPtr<AEMBResourceNode>* Existing = PlacedNodes.Find(Id))
	{
		if (Existing->Get() == Node)
		{
			PlacedNodes.Remove(Id);
		}
	}
	// L'entrée de réapparition est conservée : le nœud peut revenir par le streaming.
	if (FRespawnEntry* Entry = Respawns.Find(Id))
	{
		if (Entry->Node.Get() == Node)
		{
			Entry->Node.Reset();
		}
	}
}

void UEMBResourceNodeSubsystem::ScheduleRespawn(AEMBResourceNode* Node, const float Seconds)
{
	UWorld* World = GetWorld();
	if (!World || !Node || Node->IsDynamicNode() || !Node->GetNodeId().IsValid())
	{
		return;
	}
	FRespawnEntry& Entry = Respawns.FindOrAdd(Node->GetNodeId());
	Entry.RespawnAtSeconds = World->GetTimeSeconds() + FMath::Max(0.f, Seconds);
	Entry.Node = Node;
	StartRespawnTimer();
}

void UEMBResourceNodeSubsystem::StartRespawnTimer()
{
	UWorld* World = GetWorld();
	if (World && !World->GetTimerManager().IsTimerActive(RespawnTimerHandle))
	{
		World->GetTimerManager().SetTimer(RespawnTimerHandle, this, &UEMBResourceNodeSubsystem::PollRespawns,
			EMBHarvestDefaults::RespawnPollSeconds, true);
	}
}

void UEMBResourceNodeSubsystem::PollRespawns()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	TArray<FGuid, TInlineAllocator<8>> Due;
	for (const TPair<FGuid, FRespawnEntry>& Pair : Respawns)
	{
		if (Pair.Value.RespawnAtSeconds <= Now)
		{
			Due.Add(Pair.Key);
		}
	}

	for (const FGuid& Id : Due)
	{
		FRespawnEntry* Entry = Respawns.Find(Id);
		if (!Entry)
		{
			continue;
		}
		AEMBResourceNode* Node = Entry->Node.Get();
		if (!Node)
		{
			// Nœud déchargé (streaming) : il se rechargera intact.
			Respawns.Remove(Id);
			continue;
		}
		if (IsRespawnBlocked(*Node))
		{
			Entry->RespawnAtSeconds = Now + EMBHarvestDefaults::RespawnDeferSeconds;
			continue;
		}
		Respawns.Remove(Id);
		Node->Respawn();
	}

	if (Respawns.IsEmpty())
	{
		World->GetTimerManager().ClearTimer(RespawnTimerHandle);
	}
}

bool UEMBResourceNodeSubsystem::IsRespawnBlocked(const AEMBResourceNode& Node) const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const FVector Location = Node.GetActorLocation();
	const float RadiusSquared = FMath::Square(EMBHarvestDefaults::RespawnBlockRadius);

	// Joueur trop proche : le nœud ne doit pas apparaître sur lui ni sous ses yeux.
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
		if (Pawn && FVector::DistSquared(Pawn->GetActorLocation(), Location) < RadiusSquared)
		{
			return true;
		}
	}

	// Pièce de construction trop proche : le nœud ne doit pas pousser à travers une base.
	if (const UEMBStructureSubsystem* Structures = UEMBStructureSubsystem::Get(this))
	{
		TArray<AEMBBuildablePiece*> Pieces;
		Structures->QueryPiecesInSphere(Location, EMBHarvestDefaults::RespawnBlockRadius, Pieces);
		if (Pieces.Num() > 0)
		{
			return true;
		}
	}
	return false;
}

// ----------------------------------------------------------------------------------------------
// Arbres qui tombent
// ----------------------------------------------------------------------------------------------

void UEMBResourceNodeSubsystem::PurgeFallingTrees()
{
	FallingTrees.RemoveAll([](const FFallingEntry& Entry)
	{
		return !Entry.Tree.IsValid();
	});
}

AEMBFallingTree* UEMBResourceNodeSubsystem::SpawnFallingTree(TSubclassOf<AEMBFallingTree> TreeClass, UStaticMesh* TreeMesh,
	const FTransform& Transform, const FVector& FallDirection, AActor* OwnerKey)
{
	UWorld* World = GetWorld();
	if (!World || !TreeMesh || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}

	// Plafonds du référentiel : 3 chutes simultanées par joueur, 12 au total.
	PurgeFallingTrees();
	if (FallingTrees.Num() >= EMBHarvestDefaults::MaxFallingTreesTotal)
	{
		return nullptr;
	}
	int32 CountForOwner = 0;
	for (const FFallingEntry& Entry : FallingTrees)
	{
		if (Entry.OwnerKey.Get() == OwnerKey)
		{
			++CountForOwner;
		}
	}
	if (CountForOwner >= EMBHarvestDefaults::MaxFallingTreesPerPlayer)
	{
		return nullptr;
	}

	UClass* ClassToSpawn = TreeClass.Get();
	if (!ClassToSpawn)
	{
		ClassToSpawn = AEMBFallingTree::StaticClass();
	}
	AEMBFallingTree* Tree = World->SpawnActorDeferred<AEMBFallingTree>(ClassToSpawn, Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Tree)
	{
		return nullptr;
	}
	// Paramètres posés AVANT FinishSpawning : ils partent dans le bunch initial de réplication.
	Tree->InitFall(TreeMesh, FallDirection, FMath::RandRange(0, 255));
	Tree->FinishSpawning(Transform);

	FFallingEntry& Added = FallingTrees.AddDefaulted_GetRef();
	Added.Tree = Tree;
	Added.OwnerKey = OwnerKey;
	return Tree;
}

// ----------------------------------------------------------------------------------------------
// Nœuds dynamiques (carcasses)
// ----------------------------------------------------------------------------------------------

AEMBResourceNode* UEMBResourceNodeSubsystem::SpawnDynamicNode(const FName NodeRowId, const FTransform& Transform, const float LifeSpanSeconds)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	EnsureTableLoaded();
	const FEMBResourceNodeRow* Row = FindRow(NodeRowId);
	if (!Row)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : SpawnDynamicNode, ligne '%s' inconnue."), *NodeRowId.ToString());
		return nullptr;
	}

	// Plafond de carcasses vivantes : la plus ancienne disparaît (évite l'accumulation lors d'un grand combat).
	DynamicNodes.RemoveAll([](const TWeakObjectPtr<AEMBResourceNode>& Candidate)
	{
		return !Candidate.IsValid();
	});
	while (DynamicNodes.Num() >= EMBHarvestDefaults::MaxDynamicNodes)
	{
		if (AEMBResourceNode* Oldest = DynamicNodes[0].Get())
		{
			Oldest->Destroy();
		}
		DynamicNodes.RemoveAt(0);
	}

	// Classe Blueprint portant le mesh de la carcasse (colonne NodeClass), préchargée avec la table : jamais de
	// chargement synchrone ici. Sinon le nœud de base (la boîte de collision de repli du nœud prend le relais).
	TSubclassOf<AEMBResourceNode> NodeClass = AEMBResourceNode::StaticClass();
	if (!Row->NodeClass.IsNull())
	{
		UClass* LoadedClass = Row->NodeClass.Get();
		if (LoadedClass && LoadedClass->IsChildOf(AEMBResourceNode::StaticClass()))
		{
			NodeClass = LoadedClass;
		}
		else
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Récolte : classe %s de '%s' pas encore chargée ou invalide, nœud de base utilisé."),
				*Row->NodeClass.ToString(), *NodeRowId.ToString());
		}
	}

	AEMBResourceNode* Node = World->SpawnActorDeferred<AEMBResourceNode>(NodeClass, Transform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Node)
	{
		return nullptr;
	}
	Node->InitDynamicNode(NodeRowId);
	Node->FinishSpawning(Transform);
	Node->SetLifeSpan(LifeSpanSeconds > 0.f ? LifeSpanSeconds : EMBHarvestDefaults::DynamicNodeLifeSpanSeconds);

	DynamicNodes.Add(Node);
	return Node;
}

// ----------------------------------------------------------------------------------------------
// Sauvegarde
// ----------------------------------------------------------------------------------------------

void UEMBResourceNodeSubsystem::ExportSaveData(FEMBResourceNodesSaveData& Out) const
{
	Out.DepletedNodes.Reset();
	const UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	Out.DepletedNodes.Reserve(Respawns.Num());
	for (const TPair<FGuid, FRespawnEntry>& Pair : Respawns)
	{
		FEMBResourceNodeTimer& Timer = Out.DepletedNodes.AddDefaulted_GetRef();
		Timer.NodeId = Pair.Key;
		Timer.RemainingSeconds = static_cast<float>(FMath::Max(0.0, Pair.Value.RespawnAtSeconds - Now));
	}
}

void UEMBResourceNodeSubsystem::ImportSaveData(const FEMBResourceNodesSaveData& In)
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();

	// Données de sauvegarde non fiables : nombre d'entrées plafonné, durées finies et bornées (0 à 24 h).
	int32 EntryCount = In.DepletedNodes.Num();
	if (EntryCount > EMBHarvestDefaults::MaxSavedNodeEntries)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : sauvegarde de %d nœuds épuisés, tronquée à %d."), EntryCount, EMBHarvestDefaults::MaxSavedNodeEntries);
		EntryCount = EMBHarvestDefaults::MaxSavedNodeEntries;
	}

	Respawns.Reset();
	for (int32 Index = 0; Index < EntryCount; ++Index)
	{
		const FEMBResourceNodeTimer& Timer = In.DepletedNodes[Index];
		if (!Timer.NodeId.IsValid() || !FMath::IsFinite(Timer.RemainingSeconds))
		{
			continue;
		}
		const float Remaining = FMath::Clamp(Timer.RemainingSeconds, 0.f, EMBHarvestDefaults::MaxSavedRemainingSeconds);
		FRespawnEntry& Entry = Respawns.FindOrAdd(Timer.NodeId);
		Entry.RespawnAtSeconds = Now + Remaining;
		Entry.Node = PlacedNodes.FindRef(Timer.NodeId); // invalide si le nœud n'est pas encore chargé
	}

	// Nœuds présents : alignés sur la sauvegarde. Les absents recevront leur état à l'enregistrement (RegisterNode).
	for (const TPair<FGuid, TWeakObjectPtr<AEMBResourceNode>>& Pair : PlacedNodes)
	{
		AEMBResourceNode* Node = Pair.Value.Get();
		if (!Node)
		{
			continue;
		}
		const bool bShouldBeDepleted = Respawns.Contains(Pair.Key);
		if (bShouldBeDepleted && !Node->IsDepleted())
		{
			Node->RestoreDepleted();
		}
		else if (!bShouldBeDepleted && Node->IsDepleted())
		{
			Node->Respawn();
		}
	}

	if (Respawns.IsEmpty())
	{
		World->GetTimerManager().ClearTimer(RespawnTimerHandle);
	}
	else
	{
		StartRespawnTimer();
	}
}
