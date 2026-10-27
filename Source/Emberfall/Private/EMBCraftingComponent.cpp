// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBCraftingComponent.h"
#include "Emberfall.h"
#include "EMBBuildablePiece.h"
#include "EMBEquipmentComponent.h"
#include "EMBInventoryComponent.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBStructureSubsystem.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

UEMBCraftingComponent::UEMBCraftingComponent()
{
	// Pas de Tick : la file avance sur un timer serveur, uniquement tant qu'elle n'est pas vide.
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);

	RecipeTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_Recipes.DT_Recipes")));
}

// ---------------------------------------------------------------- Cycle de vie

void UEMBCraftingComponent::BeginPlay()
{
	Super::BeginPlay();

	// Le serveur simule ; le client propriétaire a besoin de la table pour afficher durées et progression.
	if (GetOwnerRole() != ROLE_SimulatedProxy)
	{
		EnsureRecipeTable();
	}
	if (HasAuthorityOwner())
	{
		ResolveDependencies();
		StartQueueTimerIfNeeded(); // File restaurée par ImportSaveData avant le BeginPlay.
	}
}

void UEMBCraftingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopQueueTimer();
	Super::EndPlay(EndPlayReason);
}

void UEMBCraftingComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// La file est privée : propriétaire seul.
	DOREPLIFETIME_CONDITION(UEMBCraftingComponent, Queue, COND_OwnerOnly);
}

bool UEMBCraftingComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

void UEMBCraftingComponent::EnsureRecipeTable()
{
	if (LoadedRecipes || RecipeTable.IsNull())
	{
		return;
	}
	LoadedRecipes = RecipeTable.LoadSynchronous();
	if (!LoadedRecipes)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent: DT_Recipes introuvable (%s) : aucune fabrication possible."), *RecipeTable.ToString());
	}
}

void UEMBCraftingComponent::ResolveDependencies()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}
	if (!Inventory)
	{
		Inventory = Owner->FindComponentByClass<UEMBInventoryComponent>();
	}
	if (!Equipment)
	{
		Equipment = Owner->FindComponentByClass<UEMBEquipmentComponent>();
	}
}

const FEMBRecipeRow* UEMBCraftingComponent::FindRecipe(FName RecipeId) const
{
	if (!LoadedRecipes || RecipeId.IsNone())
	{
		return nullptr;
	}
	return LoadedRecipes->FindRow<FEMBRecipeRow>(RecipeId, TEXT("UEMBCraftingComponent::FindRecipe"), /*bWarnIfRowMissing*/ false);
}

// ---------------------------------------------------------------- Temps et progression

double UEMBCraftingComponent::GetServerNow() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return 0.0;
	}
	if (const AGameStateBase* GameState = World->GetGameState())
	{
		return GameState->GetServerWorldTimeSeconds();
	}
	return World->GetTimeSeconds();
}

float UEMBCraftingComponent::ComputeProgressSeconds(const FEMBCraftQueueEntry& Entry, double Now)
{
	float Progress = Entry.AccumulatedSeconds;
	if (Entry.RunningSinceServerTime >= 0.0)
	{
		Progress += static_cast<float>(FMath::Max(0.0, Now - Entry.RunningSinceServerTime));
	}
	return Progress;
}

float UEMBCraftingComponent::GetEntryProgress(int32 QueueIndex) const
{
	if (QueueIndex != 0 || !Queue.IsValidIndex(0))
	{
		return 0.f; // Seule la tête de file progresse.
	}
	const FEMBRecipeRow* Recipe = FindRecipe(Queue[0].RecipeId);
	if (!Recipe)
	{
		return 0.f;
	}
	const float CraftTime = FMath::IsFinite(Recipe->CraftTime) ? FMath::Max(0.f, Recipe->CraftTime) : 0.f;
	if (CraftTime <= KINDA_SMALL_NUMBER)
	{
		return 1.f;
	}
	return FMath::Clamp(ComputeProgressSeconds(Queue[0], GetServerNow()) / CraftTime, 0.f, 1.f);
}

bool UEMBCraftingComponent::IsEntryPaused(int32 QueueIndex) const
{
	return QueueIndex != 0 || !Queue.IsValidIndex(QueueIndex) || Queue[QueueIndex].RunningSinceServerTime < 0.0;
}

// ---------------------------------------------------------------- Validations

bool UEMBCraftingComponent::IsGlyphUnlocked(FName GlyphId) const
{
	if (GlyphId.IsNone())
	{
		return true;
	}
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const AEMBPlayerState* PlayerState = Pawn ? Pawn->GetPlayerState<AEMBPlayerState>() : nullptr;
	const UEMBProgressionComponent* Progression = PlayerState ? PlayerState->GetProgressionComponent() : nullptr;
	return Progression && Progression->HasGlyph(GlyphId);
}

bool UEMBCraftingComponent::IsStationAvailable(FName StationRow) const
{
	if (StationRow.IsNone())
	{
		return true; // Fabrication à la main.
	}
	const AActor* Owner = GetOwner();
	UEMBStructureSubsystem* Structures = UEMBStructureSubsystem::Get(this);
	if (!Owner || !Structures)
	{
		return false;
	}

	TArray<AEMBBuildablePiece*> Nearby;
	Structures->QueryPiecesInSphere(Owner->GetActorLocation(), EMBItemDefaults::StationRadius, Nearby);
	for (const AEMBBuildablePiece* Piece : Nearby)
	{
		if (IsValid(Piece) && Piece->GetPieceRowName() == StationRow)
		{
			return true;
		}
	}
	return false;
}

bool UEMBCraftingComponent::IsStationAvailableThrottled(FName StationRow, double Now, bool bRunning)
{
	if (StationRow.IsNone())
	{
		return true;
	}
	// En progression : contrôle à chaque passage (pause immédiate si l'atelier disparaît). En pause : au plus 1 fois par seconde.
	if (bRunning || StationRow != CachedStationName || Now - LastStationCheckTime >= 1.0)
	{
		CachedStationName = StationRow;
		bCachedStationResult = IsStationAvailable(StationRow);
		LastStationCheckTime = Now;
	}
	return bCachedStationResult;
}

bool UEMBCraftingComponent::IsRecipeUsable(const FEMBRecipeRow& Recipe, bool bCheckOutput) const
{
	if (!FMath::IsFinite(Recipe.CraftTime))
	{
		return false;
	}
	for (const TPair<FName, int32>& Ingredient : Recipe.Ingredients)
	{
		if (Ingredient.Key.IsNone() || Ingredient.Value <= 0 || Ingredient.Value > EMBItemDefaults::MaxIngredientAmount)
		{
			return false;
		}
	}
	if (Recipe.bIsRepair)
	{
		return !Recipe.RepairTargetItem.IsNone();
	}
	if (Recipe.OutputId.IsNone() || Recipe.OutputCount < 1 || Recipe.OutputCount > EMBItemDefaults::MaxIngredientAmount)
	{
		return false;
	}
	if (Recipe.bOutputIsItem && bCheckOutput)
	{
		return Equipment && Equipment->FindItemRow(Recipe.OutputId) && Recipe.OutputCount <= EMBItemDefaults::MaxItemInstances;
	}
	return true;
}

bool UEMBCraftingComponent::BuildScaledCost(const FEMBRecipeRow& Recipe, int32 Count, TMap<FName, int32>& OutCost) const
{
	OutCost.Reset();
	for (const TPair<FName, int32>& Ingredient : Recipe.Ingredients)
	{
		const int64 Total = static_cast<int64>(Ingredient.Value) * static_cast<int64>(Count);
		if (Total <= 0 || Total > EMBItemDefaults::MaxTotalIngredientAmount)
		{
			return false;
		}
		OutCost.Add(Ingredient.Key, static_cast<int32>(Total));
	}
	return true;
}

void UEMBCraftingComponent::RefundIngredients(const FEMBRecipeRow& Recipe, int32 Count)
{
	if (!Inventory)
	{
		return;
	}
	TMap<FName, int32> Cost;
	if (!BuildScaledCost(Recipe, Count, Cost))
	{
		return;
	}
	for (const TPair<FName, int32>& Ingredient : Cost)
	{
		Inventory->AddResource(Ingredient.Key, Ingredient.Value);
	}
}

// ---------------------------------------------------------------- Mise en file et annulation

bool UEMBCraftingComponent::RequestCraft_Validate(FName RecipeId, int32 Count)
{
	return Count >= 1 && Count <= EMBItemDefaults::MaxCraftCountPerRequest;
}

bool UEMBCraftingComponent::ConsumeRpcBudget()
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (Now - RpcWindowStart >= 1.0)
	{
		RpcWindowStart = Now;
		RpcWindowCount = 0;
	}
	return ++RpcWindowCount <= 10; // ~10 demandes par seconde : l'excédent est ignoré.
}

void UEMBCraftingComponent::RequestCraft_Implementation(FName RecipeId, int32 Count)
{
	if (!HasAuthorityOwner() || !ConsumeRpcBudget())
	{
		return;
	}
	const EEMBCraftResult Result = TryEnqueue(RecipeId, Count);
	if (Result != EEMBCraftResult::Ok)
	{
		Client_CraftRejected(RecipeId, Result);
	}
}

EEMBCraftResult UEMBCraftingComponent::TryEnqueue(FName RecipeId, int32 Count)
{
	if (!HasAuthorityOwner())
	{
		return EEMBCraftResult::InvalidRequest;
	}
	EnsureRecipeTable();
	ResolveDependencies();

	if (Count < 1 || Count > EMBItemDefaults::MaxCraftCountPerRequest || !Inventory || !Equipment)
	{
		return EEMBCraftResult::InvalidRequest;
	}
	const FEMBRecipeRow* Recipe = FindRecipe(RecipeId);
	if (!Recipe)
	{
		return EEMBCraftResult::UnknownRecipe;
	}
	if (!IsRecipeUsable(*Recipe, true))
	{
		return EEMBCraftResult::InvalidRecipe;
	}
	if (!IsGlyphUnlocked(Recipe->RequiredGlyph))
	{
		return EEMBCraftResult::MissingGlyph;
	}
	if (Queue.Num() >= EMBItemDefaults::MaxQueueEntries)
	{
		return EEMBCraftResult::QueueFull;
	}

	if (Recipe->bIsRepair)
	{
		// Il faut autant d'instances abîmées que d'exemplaires demandés, en comptant les réparations déjà en file.
		int32 PendingRepairs = 0;
		for (const FEMBCraftQueueEntry& Entry : Queue)
		{
			const FEMBRecipeRow* Queued = FindRecipe(Entry.RecipeId);
			if (Queued && Queued->bIsRepair && Queued->RepairTargetItem == Recipe->RepairTargetItem)
			{
				PendingRepairs += Entry.Remaining;
			}
		}
		if (Equipment->CountDamagedInstances(Recipe->RepairTargetItem) - PendingRepairs < Count)
		{
			return EEMBCraftResult::NothingToRepair;
		}
	}
	else if (Recipe->bOutputIsItem)
	{
		// Place dans le sac pour toute la file : les exemplaires déjà en attente réservent leurs emplacements.
		int64 Reserved = 0;
		for (const FEMBCraftQueueEntry& Entry : Queue)
		{
			const FEMBRecipeRow* Queued = FindRecipe(Entry.RecipeId);
			if (Queued && !Queued->bIsRepair && Queued->bOutputIsItem)
			{
				Reserved += static_cast<int64>(Entry.Remaining) * Queued->OutputCount;
			}
		}
		const int64 Needed = static_cast<int64>(Recipe->OutputCount) * Count;
		if (static_cast<int64>(Equipment->GetBagFreeSlots()) < Reserved + Needed)
		{
			return EEMBCraftResult::BagFull;
		}
	}

	TMap<FName, int32> Cost;
	if (!BuildScaledCost(*Recipe, Count, Cost))
	{
		return EEMBCraftResult::InvalidRecipe;
	}
	// Consommation atomique : si une seule ressource manque, rien n'est retiré.
	if (!Inventory->HasResources(Cost) || !Inventory->ConsumeResources(Cost))
	{
		return EEMBCraftResult::MissingIngredients;
	}

	FEMBCraftQueueEntry NewEntry;
	NewEntry.RecipeId = RecipeId;
	NewEntry.Remaining = Count;
	NewEntry.AccumulatedSeconds = 0.f;
	NewEntry.RunningSinceServerTime = -1.0;
	Queue.Add(NewEntry);

	NotifyQueueChanged();
	StartQueueTimerIfNeeded();
	return EEMBCraftResult::Ok;
}

bool UEMBCraftingComponent::RequestCancel_Validate(int32 QueueIndex, FName RecipeId)
{
	return QueueIndex >= 0 && QueueIndex < EMBItemDefaults::MaxQueueEntries;
}

void UEMBCraftingComponent::RequestCancel_Implementation(int32 QueueIndex, FName RecipeId)
{
	if (!HasAuthorityOwner() || !ConsumeRpcBudget())
	{
		return;
	}
	if (!Queue.IsValidIndex(QueueIndex) || Queue[QueueIndex].RecipeId != RecipeId)
	{
		return; // Index périmé ou entrée différente (la file a avancé entre-temps) : ignoré sans sanction.
	}
	ResolveDependencies();
	EnsureRecipeTable();

	const FEMBCraftQueueEntry Cancelled = Queue[QueueIndex];
	if (const FEMBRecipeRow* Recipe = FindRecipe(Cancelled.RecipeId))
	{
		RefundIngredients(*Recipe, Cancelled.Remaining); // Remboursement intégral, exemplaire en cours compris.
	}
	Queue.RemoveAt(QueueIndex);

	NotifyQueueChanged();
	if (Queue.IsEmpty())
	{
		StopQueueTimer();
	}
}

void UEMBCraftingComponent::Client_CraftRejected_Implementation(FName RecipeId, EEMBCraftResult Reason)
{
	OnCraftRejected.Broadcast(RecipeId, Reason);
}

// ---------------------------------------------------------------- Timer serveur

void UEMBCraftingComponent::StartQueueTimerIfNeeded()
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthorityOwner() || Queue.IsEmpty())
	{
		return;
	}
	FTimerManager& Timers = World->GetTimerManager();
	if (!Timers.IsTimerActive(QueueTimerHandle))
	{
		Timers.SetTimer(QueueTimerHandle, this, &UEMBCraftingComponent::ProcessQueue, EMBItemDefaults::CraftTickInterval, true, 0.f);
	}
}

void UEMBCraftingComponent::StopQueueTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(QueueTimerHandle);
	}
}

bool UEMBCraftingComponent::DeliverOneUnit(const FEMBRecipeRow& Recipe)
{
	if (!Inventory || !Equipment)
	{
		return false;
	}

	if (Recipe.bIsRepair)
	{
		if (!Equipment->RepairFirstInstance(Recipe.RepairTargetItem))
		{
			// L'objet a disparu entre-temps : l'exemplaire est rendu, sans XP.
			RefundIngredients(Recipe, 1);
			return true;
		}
	}
	else if (Recipe.bOutputIsItem)
	{
		if (!Equipment->FindItemRow(Recipe.OutputId))
		{
			// Sortie validée seulement ici (import de sauvegarde sans validation) : objet inconnu, l'exemplaire est rendu.
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent: objet '%s' inconnu de DT_Items, ingredients rembourses."), *Recipe.OutputId.ToString());
			RefundIngredients(Recipe, 1);
			return true;
		}
		if (Equipment->GetBagFreeSlots() < Recipe.OutputCount)
		{
			return false; // Sac plein : l'exemplaire terminé attend de la place.
		}
		for (int32 Index = 0; Index < Recipe.OutputCount; ++Index)
		{
			Equipment->AddItemInstance(Recipe.OutputId);
		}
	}
	else
	{
		Inventory->AddResource(Recipe.OutputId, Recipe.OutputCount);
	}

	if (!Recipe.bIsRepair && Recipe.XP > 0) // Une réparation ne rapporte jamais d'XP, quelle que soit la donnée.
	{
		const APawn* Pawn = Cast<APawn>(GetOwner());
		const AEMBPlayerState* PlayerState = Pawn ? Pawn->GetPlayerState<AEMBPlayerState>() : nullptr;
		if (UEMBProgressionComponent* Progression = PlayerState ? PlayerState->GetProgressionComponent() : nullptr)
		{
			Progression->AddXP(static_cast<int64>(Recipe.XP));
		}
	}
	return true;
}

void UEMBCraftingComponent::ProcessQueue()
{
	if (!HasAuthorityOwner())
	{
		StopQueueTimer();
		return;
	}
	EnsureRecipeTable();
	ResolveDependencies();

	bool bChanged = false;

	for (int32 Step = 0; Step < EMBItemDefaults::MaxCraftStepsPerTick && !Queue.IsEmpty(); ++Step)
	{
		const FEMBCraftQueueEntry Head = Queue[0]; // Copie : la file est modifiée plus bas.
		const FEMBRecipeRow* Recipe = FindRecipe(Head.RecipeId);
		if (!Recipe || Head.Remaining <= 0)
		{
			if (!Recipe)
			{
				UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent: recette '%s' disparue de la table, entree retiree (ingredients non remboursables)."), *Head.RecipeId.ToString());
			}
			Queue.RemoveAt(0);
			bChanged = true;
			continue;
		}

		const double Now = GetServerNow();
		const float CraftTime = FMath::IsFinite(Recipe->CraftTime) ? FMath::Max(0.f, Recipe->CraftTime) : 0.f;
		const bool bStationAvailable = IsStationAvailableThrottled(Recipe->RequiredStation, Now, Head.RunningSinceServerTime >= 0.0);
		const float Progress = ComputeProgressSeconds(Head, Now);

		if (Progress < CraftTime)
		{
			// En cours : démarre ou met en pause selon la présence de l'atelier.
			FEMBCraftQueueEntry& Live = Queue[0];
			if (bStationAvailable && Live.RunningSinceServerTime < 0.0)
			{
				Live.RunningSinceServerTime = Now;
				bChanged = true;
			}
			else if (!bStationAvailable && Live.RunningSinceServerTime >= 0.0)
			{
				Live.AccumulatedSeconds = Progress;
				Live.RunningSinceServerTime = -1.0;
				bChanged = true;
			}
			break;
		}

		// Exemplaire terminé : l'atelier doit encore être là au moment de la livraison.
		if (!bStationAvailable)
		{
			FEMBCraftQueueEntry& Live = Queue[0];
			if (Live.RunningSinceServerTime >= 0.0 || !FMath::IsNearlyEqual(Live.AccumulatedSeconds, CraftTime))
			{
				Live.AccumulatedSeconds = CraftTime;
				Live.RunningSinceServerTime = -1.0;
				bChanged = true;
			}
			break;
		}

		// Exemplaire terminé : livraison.
		if (!DeliverOneUnit(*Recipe))
		{
			FEMBCraftQueueEntry& Live = Queue[0];
			if (Live.RunningSinceServerTime >= 0.0 || !FMath::IsNearlyEqual(Live.AccumulatedSeconds, CraftTime))
			{
				Live.AccumulatedSeconds = CraftTime; // Figé à 100 % jusqu'à ce qu'il y ait de la place.
				Live.RunningSinceServerTime = -1.0;
				bChanged = true;
			}
			break;
		}

		bChanged = true;
		if (Head.Remaining <= 1)
		{
			Queue.RemoveAt(0);
			continue;
		}

		// Exemplaire suivant : on reporte le dépassement de temps (au plus deux périodes de timer) pour ne pas perdre de précision.
		FEMBCraftQueueEntry& Next = Queue[0];
		Next.Remaining = Head.Remaining - 1;
		Next.AccumulatedSeconds = 0.f;
		const float Overshoot = FMath::Clamp(Progress - CraftTime, 0.f, EMBItemDefaults::CraftTickInterval * 2.f);
		Next.RunningSinceServerTime = bStationAvailable ? Now - static_cast<double>(Overshoot) : -1.0;
	}

	if (bChanged)
	{
		NotifyQueueChanged();
	}
	if (Queue.IsEmpty())
	{
		StopQueueTimer();
	}
}

// ---------------------------------------------------------------- Notification et réplication

void UEMBCraftingComponent::NotifyQueueChanged()
{
	OnCraftingQueueChanged.Broadcast();
}

void UEMBCraftingComponent::OnRep_Queue()
{
	OnCraftingQueueChanged.Broadcast();
}

// ---------------------------------------------------------------- Sauvegarde

void UEMBCraftingComponent::ExportSaveData(FEMBCraftingSaveData& Out) const
{
	Out.Entries.Reset();
	const double Now = GetServerNow();
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		FEMBCraftQueueEntry Saved = Queue[Index];
		Saved.AccumulatedSeconds = (Index == 0) ? ComputeProgressSeconds(Saved, Now) : 0.f;
		Saved.RunningSinceServerTime = -1.0; // Relatif à la session : non sauvegardé.
		Out.Entries.Add(Saved);
	}
}

void UEMBCraftingComponent::ImportSaveData(const FEMBCraftingSaveData& In)
{
	if (!HasAuthorityOwner())
	{
		return;
	}
	EnsureRecipeTable();
	ResolveDependencies();
	if (!LoadedRecipes)
	{
		// Sans table, toute recette serait jugée inconnue : on garde la file courante plutôt que de perdre des ingrédients.
		UE_LOG(LogEmberfall, Error, TEXT("UEMBCraftingComponent::ImportSaveData: DT_Recipes indisponible, import annule."));
		return;
	}

	TArray<FEMBCraftQueueEntry> NewQueue;
	for (const FEMBCraftQueueEntry& Saved : In.Entries)
	{
		if (NewQueue.Num() >= EMBItemDefaults::MaxQueueEntries)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent::ImportSaveData: file trop longue, entrees excedentaires ignorees."));
			break;
		}
		const FEMBRecipeRow* Recipe = FindRecipe(Saved.RecipeId);
		if (!Recipe)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent::ImportSaveData: recette '%s' inconnue, entree ignoree (ingredients non remboursables)."), *Saved.RecipeId.ToString());
			continue;
		}
		// La sortie n'est pas validée ici (la table d'objets peut ne pas être prête) : elle l'est à la livraison.
		if (!IsRecipeUsable(*Recipe, false))
		{
			const int32 Lost = FMath::Clamp(Saved.Remaining, 1, EMBItemDefaults::MaxCraftCountPerRequest);
			if (Inventory)
			{
				RefundIngredients(*Recipe, Lost);
			}
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBCraftingComponent::ImportSaveData: recette '%s' invalide, entree ignoree (%s)."), *Saved.RecipeId.ToString(), Inventory ? TEXT("ingredients rembourses") : TEXT("ingredients perdus"));
			continue;
		}

		const float CraftTime = FMath::Max(0.f, Recipe->CraftTime);
		FEMBCraftQueueEntry Clean;
		Clean.RecipeId = Saved.RecipeId;
		Clean.Remaining = FMath::Clamp(Saved.Remaining, 1, EMBItemDefaults::MaxCraftCountPerRequest);
		Clean.AccumulatedSeconds = (NewQueue.IsEmpty() && FMath::IsFinite(Saved.AccumulatedSeconds)) ? FMath::Clamp(Saved.AccumulatedSeconds, 0.f, CraftTime) : 0.f;
		Clean.RunningSinceServerTime = -1.0;
		NewQueue.Add(Clean);
	}

	Queue = MoveTemp(NewQueue);
	NotifyQueueChanged();
	if (Queue.IsEmpty())
	{
		StopQueueTimer();
	}
	else
	{
		StartQueueTimerIfNeeded();
	}
}
