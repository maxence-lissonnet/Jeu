// Emberfall - sauvegarde du monde, des joueurs et des constructions (Dev B, référentiel section 8).

#include "EMBSaveSubsystem.h"

#include "Emberfall.h"
#include "EMBBuildablePiece.h"
#include "EMBBuildingTypes.h"
#include "EMBCombatComponent.h"
#include "EMBDragonBoss.h"
#include "EMBGameplayTags.h"
#include "EMBInventoryComponent.h"
#include "EMBPlayerCharacter.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBProgressionSettings.h"
#include "EMBProgressionSubsystem.h"
#include "EMBSaveGame.h"
#include "EMBStatsComponent.h"
#include "EMBStructureSubsystem.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	constexpr int32 SaveUserIndex = 0;
	constexpr float PawnRestoreRetryPeriod = 0.25f;

	/** Bornes de validation d'une sauvegarde (un fichier peut être corrompu ou édité à la main). */
	constexpr int32 MaxSavedPlayers = 256;
	constexpr double MaxScaleDeviation = 0.01;

	/**
	 * Identifiant court pour les journaux : jamais l'identifiant Steam/EOS ni le nom du joueur en clair.
	 * (Hachage 32 bits : suffisant pour corréler deux lignes de log, insuffisant pour retrouver le joueur.)
	 */
	FString ShortId(const FString& StableId)
	{
		return FString::Printf(TEXT("%08X"), GetTypeHash(StableId));
	}

	/** Ne garde que lettres et chiffres : le nom entre dans une clé de sauvegarde. */
	FString SanitizeName(const FString& Name)
	{
		FString Result;
		Result.Reserve(Name.Len());
		for (const TCHAR Character : Name)
		{
			Result.AppendChar(FChar::IsAlnum(Character) ? Character : TEXT('_'));
		}
		return Result.IsEmpty() ? FString(TEXT("Unknown")) : Result;
	}

	/** Transform exploitable : pas de NaN, échelle ~1, coordonnées dans les limites du monde. */
	bool IsTransformSane(const FTransform& Transform)
	{
		if (Transform.ContainsNaN())
		{
			return false;
		}
		const FVector Scale = Transform.GetScale3D();
		if (FMath::Abs(Scale.X - 1.0) > MaxScaleDeviation || FMath::Abs(Scale.Y - 1.0) > MaxScaleDeviation || FMath::Abs(Scale.Z - 1.0) > MaxScaleDeviation)
		{
			return false;
		}
		const FVector Location = Transform.GetLocation();
		constexpr double Limit = EMBBuildingDefaults::MaxWorldCoordinate;
		return FMath::Abs(Location.X) <= Limit && FMath::Abs(Location.Y) <= Limit && FMath::Abs(Location.Z) <= Limit;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------------------------------------------------

void UEMBSaveSubsystem::Deinitialize()
{
	if (UWorld* World = BoundWorld.Get())
	{
		FTimerManager& TimerManager = World->GetTimerManager();
		TimerManager.ClearTimer(AutosaveTimerHandle);
		TimerManager.ClearTimer(SafePointTimerHandle);
		TimerManager.ClearTimer(RestoreRetryTimerHandle);
	}

	Super::Deinitialize();
}

UWorld* UEMBSaveSubsystem::GetServerWorld() const
{
	return BoundWorld.Get();
}

bool UEMBSaveSubsystem::IsServer() const
{
	const UWorld* World = GetServerWorld();
	return World && World->GetNetMode() != NM_Client;
}

void UEMBSaveSubsystem::BindToWorld(UWorld* World)
{
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// Jamais de remise à zéro si le monde lié est déjà celui-ci : un deuxième appel ne doit pas effacer un chargement.
	if (BoundWorld.Get() == World)
	{
		return;
	}

	// Remise à zéro SYNCHRONE : aucun état du monde précédent (joueurs, constructions, chargement) ne survit.
	// L'incrément de génération invalide tout callback asynchrone encore en vol pour l'ancien monde.
	++LoadGeneration;
	LoadState = ELoadState::NotLoaded;
	PendingLoadCount = 0;
	LoadedSlotA = nullptr;
	LoadedSlotB = nullptr;
	LoadWorld.Reset();
	PlayerCache.Reset();
	LastSafeTransforms.Reset();
	bNextWriteToSlotA = true;
	ControllerIds.Reset();
	ActiveIds.Reset();
	UnpersistedControllers.Reset();
	PendingRestoreControllers.Reset();
	PawnRestoreRetries.Reset();
	bSavePending = false;
	bSaveDeferredByBoss = false;
	bBlockedByUnusableSave = false;
	// bWriteInFlight n'est volontairement PAS remis à zéro : une écriture de l'ancien monde peut encore tourner, et une
	// seule écriture à la fois est autorisée ; son callback (OnSlotSaved) le remettra à false.

	StartWorldTimers(World);
}

void UEMBSaveSubsystem::StartWorldTimers(UWorld* World)
{
	if (UWorld* OldWorld = BoundWorld.Get())
	{
		FTimerManager& OldTimers = OldWorld->GetTimerManager();
		OldTimers.ClearTimer(AutosaveTimerHandle);
		OldTimers.ClearTimer(SafePointTimerHandle);
		OldTimers.ClearTimer(RestoreRetryTimerHandle);
	}

	BoundWorld = World;
	FTimerManager& TimerManager = World->GetTimerManager();
	// Chaque callback vérifie IsServer() : un client pur ne fait rien.
	TimerManager.SetTimer(AutosaveTimerHandle, this, &UEMBSaveSubsystem::HandleAutosaveTimer, FMath::Max(AutosaveIntervalSeconds, 10.f), /*bLoop*/ true);
	TimerManager.SetTimer(SafePointTimerHandle, this, &UEMBSaveSubsystem::HandleSafePointTimer, FMath::Max(SafePointIntervalSeconds, 1.f), /*bLoop*/ true);

	// Autosave à la mort d'un boss : on écoute l'enregistrement des événements monde.
	if (UEMBProgressionSubsystem* Progression = World->GetSubsystem<UEMBProgressionSubsystem>())
	{
		Progression->OnWorldEventRecorded.AddUniqueDynamic(this, &UEMBSaveSubsystem::HandleWorldEventRecorded);
	}
}

void UEMBSaveSubsystem::SetActiveSlot(int32 SlotIndex)
{
	const int32 NewSlot = FMath::Clamp(SlotIndex, 0, 2);
	if (LoadState != ELoadState::NotLoaded && NewSlot != ActiveSlot)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("SetActiveSlot(%d) ignore : le monde %d est deja en cours de chargement ou charge."), NewSlot, ActiveSlot);
		return;
	}
	ActiveSlot = NewSlot;
}

FString UEMBSaveSubsystem::BuildSlotName(bool bSlotA) const
{
	return FString::Printf(TEXT("EMB_World_%d_%s"), ActiveSlot, bSlotA ? TEXT("A") : TEXT("B"));
}

// ---------------------------------------------------------------------------------------------------------------------
// Identifiant stable
// ---------------------------------------------------------------------------------------------------------------------

FString UEMBSaveSubsystem::GetStablePlayerId(const APlayerState* PS)
{
	if (!PS)
	{
		return FString();
	}

	// 1) Vrai service en ligne (Steam, EOS...) : l'identifiant est stable entre sessions et entre machines.
	//    L'Online Subsystem NULL fabrique des identifiants valides mais éphémères : on ne s'y fie pas.
	const FUniqueNetIdRepl& NetId = PS->GetUniqueId();
	if (NetId.IsValid())
	{
		const FName IdType = NetId.GetType();
		if (IdType != NAME_None && IdType != FName(TEXT("NULL")))
		{
			const FString IdString = NetId.ToString();
			if (!IdString.IsEmpty())
			{
				return IdString;
			}
		}
	}

	// 2) Hôte local d'une partie en écoute (ou jeu solo) : clé fixe.
	const APlayerController* Owner = Cast<APlayerController>(PS->GetOwner());
	if (Owner && Owner->IsLocalController() && PS->GetNetMode() != NM_Client)
	{
		return TEXT("LocalHost");
	}

	// 3) Invité : identifié par son nom (non garanti unique, voir la documentation).
	return FString(TEXT("Guest_")) + SanitizeName(PS->GetPlayerName());
}

TSoftObjectPtr<UDataTable> UEMBSaveSubsystem::GetConfiguredBuildPiecesTable()
{
	// Les propriétés Config sont chargées dans l'objet par défaut de la classe : accessibles sans instance.
	const UEMBSaveSubsystem* Defaults = GetDefault<UEMBSaveSubsystem>();
	return Defaults ? Defaults->BuildPiecesTable : TSoftObjectPtr<UDataTable>();
}

// ---------------------------------------------------------------------------------------------------------------------
// Chargement
// ---------------------------------------------------------------------------------------------------------------------

void UEMBSaveSubsystem::LoadActiveSlot()
{
	UWorld* World = GetServerWorld();
	// Idempotent : sans effet si un chargement est en cours ou déjà fait pour le monde lié.
	if (!World || !IsServer() || LoadState != ELoadState::NotLoaded)
	{
		return;
	}

	LoadState = ELoadState::Loading;
	LoadWorld = World;
	LoadedSlotA = nullptr;
	LoadedSlotB = nullptr;
	PendingLoadCount = 2;
	const uint32 Generation = LoadGeneration;

	// Lecture des DEUX fichiers : le plus récent valide l'emporte (une écriture interrompue laisse l'autre intact).
	// La génération est capturée : si le monde change avant la fin, le callback est ignoré.
	const FString NameA = BuildSlotName(true);
	const FString NameB = BuildSlotName(false);
	UGameplayStatics::AsyncLoadGameFromSlot(NameA, SaveUserIndex, FAsyncLoadGameFromSlotDelegate::CreateWeakLambda(this,
		[this, Generation](const FString& SlotName, const int32 /*UserIndex*/, USaveGame* LoadedData)
		{
			OnSlotLoaded(Generation, SlotName, LoadedData);
		}));
	UGameplayStatics::AsyncLoadGameFromSlot(NameB, SaveUserIndex, FAsyncLoadGameFromSlotDelegate::CreateWeakLambda(this,
		[this, Generation](const FString& SlotName, const int32 /*UserIndex*/, USaveGame* LoadedData)
		{
			OnSlotLoaded(Generation, SlotName, LoadedData);
		}));
}

void UEMBSaveSubsystem::OnSlotLoaded(uint32 Generation, const FString& SlotName, USaveGame* LoadedData)
{
	// Callback périmé : le monde a été relié (nouvelle génération) ou a disparu pendant la lecture.
	if (Generation != LoadGeneration || LoadState != ELoadState::Loading || !LoadWorld.IsValid() || LoadWorld.Get() != BoundWorld.Get())
	{
		return;
	}

	// Un fichier absent ou illisible arrive ici avec LoadedData == nullptr : ce n'est pas une erreur.
	UEMBSaveGame* Save = Cast<UEMBSaveGame>(LoadedData);
	if (SlotName == BuildSlotName(true))
	{
		LoadedSlotA = Save;
	}
	else if (SlotName == BuildSlotName(false))
	{
		LoadedSlotB = Save;
	}

	if (--PendingLoadCount <= 0)
	{
		FinishLoading();
	}
}

void UEMBSaveSubsystem::FinishLoading()
{
	UWorld* World = LoadWorld.Get();
	if (!World || World != BoundWorld.Get())
	{
		// Le monde a disparu pendant la lecture : on revient à l'état initial, rien n'est appliqué.
		LoadState = ELoadState::NotLoaded;
		LoadedSlotA = nullptr;
		LoadedSlotB = nullptr;
		return;
	}

	// Validation de chaque fichier : bon monde, version migrable, écriture complète.
	auto Evaluate = [this](UEMBSaveGame* Save, const TCHAR* Label) -> UEMBSaveGame*
	{
		if (!Save)
		{
			return nullptr;
		}
		if (Save->SlotIndex != ActiveSlot)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde %s ignoree : elle appartient au monde %d (monde actif %d)."), Label, Save->SlotIndex, ActiveSlot);
			return nullptr;
		}
		if (!Save->MigrateToCurrent())
		{
			// Le fichier existe mais ce binaire ne sait pas le lire (version plus récente ou migration impossible).
			// Écrire dans l'AUTRE fichier est sans danger, mais écrire un jour dans CELUI-CI le détruirait : on bloque.
			bBlockedByUnusableSave = true;
			UE_LOG(LogEmberfall, Error, TEXT("Sauvegarde %s du monde %d illisible par cette version du jeu : AUCUNE ecriture ne sera faite pour ne pas la detruire."), Label, ActiveSlot);
			return nullptr;
		}
		if (!Save->IsComplete())
		{
			// Fichier tronqué (écriture interrompue) : on l'ignore, l'autre fichier prend le relais.
			UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde %s du monde %d incomplete (ecriture interrompue ?) : ignoree."), Label, ActiveSlot);
			return nullptr;
		}
		return Save;
	};

	UEMBSaveGame* SlotA = Evaluate(LoadedSlotA, TEXT("A"));
	UEMBSaveGame* SlotB = Evaluate(LoadedSlotB, TEXT("B"));

	UEMBSaveGame* Best = nullptr;
	if (SlotA && SlotB)
	{
		Best = SlotA->IsNewerThan(SlotB) ? SlotA : SlotB;
	}
	else
	{
		Best = SlotA ? SlotA : SlotB;
	}

	// Retenu = A : on écrit B. Retenu = B ou aucun : on écrit A.
	bNextWriteToSlotA = !(Best != nullptr && Best == SlotA);

	if (Best)
	{
		ApplyLoadedSave(World, Best);
	}
	else
	{
		UE_LOG(LogEmberfall, Log, TEXT("Aucune sauvegarde valide pour le monde %d : monde vierge."), ActiveSlot);
	}

	LoadedSlotA = nullptr;
	LoadedSlotB = nullptr;
	LoadState = ELoadState::Loaded;
	OnLoadCompleted.Broadcast(Best != nullptr);

	// Joueurs arrivés pendant le chargement.
	ProcessPendingRestores();
}

void UEMBSaveSubsystem::ApplyLoadedSave(UWorld* World, UEMBSaveGame* Save)
{
	// Joueurs : on garde TOUTES les entrées valides (bornées), y compris celles des joueurs absents.
	PlayerCache.Reset();
	int32 PlayerCount = 0;
	for (const FEMBPlayerSaveData& Player : Save->Players)
	{
		if (PlayerCount >= MaxSavedPlayers)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde : plus de %d joueurs, les suivants sont ignores."), MaxSavedPlayers);
			break;
		}
		if (Player.StableId.IsEmpty())
		{
			continue;
		}
		PlayerCache.Add(Player.StableId, Player);
		if (Player.bHasLastSafeTransform && IsTransformSane(Player.LastSafeTransform))
		{
			LastSafeTransforms.Add(Player.StableId, Player.LastSafeTransform);
		}
		++PlayerCount;
	}

	// Événements monde (boss vaincus) : le sous-système de progression les redistribue sans diffuser d'événement.
	if (UEMBProgressionSubsystem* Progression = World->GetSubsystem<UEMBProgressionSubsystem>())
	{
		Progression->RestoreWorldEvents(Save->World.WorldEvents);

		// Un dragon dont la défaite est enregistrée ne doit pas réapparaître. Son BeginPlay a pu s'exécuter AVANT la fin
		// du chargement asynchrone et n'a donc rien vu : on le lui signale ici (ApplyDefeatedFromSave est idempotent).
		if (Progression->IsWorldEventRecorded(EMBTags::Event_Boss_Killed_Dragon.GetTag()))
		{
			for (TActorIterator<AEMBDragonBoss> It(World); It; ++It)
			{
				if (AEMBDragonBoss* Boss = *It)
				{
					Boss->ApplyDefeatedFromSave();
				}
			}
		}
	}

	SpawnSavedPieces(World, Save->Pieces);
}

void UEMBSaveSubsystem::SpawnSavedPieces(UWorld* World, const TArray<FEMBPieceSaveData>& SavedPieces)
{
	if (SavedPieces.IsEmpty())
	{
		return;
	}

	UEMBStructureSubsystem* Structure = World->GetSubsystem<UEMBStructureSubsystem>();
	UDataTable* Table = BuildPiecesTable.IsNull() ? nullptr : BuildPiecesTable.LoadSynchronous();
	// La table doit bien être une table de FEMBBuildPieceRow : FindRow<T> sur un autre type de ligne lirait de la mémoire invalide.
	if (Table && Table->GetRowStruct() != FEMBBuildPieceRow::StaticStruct())
	{
		UE_LOG(LogEmberfall, Error, TEXT("BuildPiecesTable n'est pas une table de FEMBBuildPieceRow."));
		Table = nullptr;
	}
	if (!Structure || !Table)
	{
		// Sans table ni sous-système, impossible de valider les pièces : on ne restaure rien ET on interdit toute
		// écriture, sinon la prochaine sauvegarde effacerait les constructions du joueur.
		bBlockedByUnusableSave = true;
		UE_LOG(LogEmberfall, Error, TEXT("Constructions non restaurees (BuildPiecesTable absente ou sous-systeme de structure indisponible) : ecritures bloquees pour ne pas perdre %d piece(s)."), SavedPieces.Num());
		return;
	}

	// COÛT : une passe synchrone qui fait apparaître N acteurs répliqués. Mesure indicative attendue : quelques
	// dizaines de microsecondes par pièce hors chargement de classes, soit quelques centaines de ms pour 5 000 pièces,
	// au démarrage du serveur (avant l'arrivée des joueurs). Une seule passe suffit en v1 ; si le profilage montre un
	// hitch au-delà de ~2 000 pièces, étaler le spawn par lots (ex. 250 pièces par frame) en gardant BeginBulkLoad ouvert
	// jusqu'à la dernière.
	TMap<FName, UClass*> ClassByRow;
	TMap<int32, AEMBBuildablePiece*> PieceBySaveId;
	TMap<const AEMBBuildablePiece*, int32> SaveIdByPiece;
	PieceBySaveId.Reserve(SavedPieces.Num());

	// Pas de propagation ni d'effondrement pendant l'enregistrement : un mur enregistré avant sa fondation
	// serait sinon vu comme non soutenu.
	Structure->BeginBulkLoad();

	int32 SkippedCount = 0;
	int32 SpawnedCount = 0; // compteur dédié du plafond (indépendant des tables d'identifiants)
	for (const FEMBPieceSaveData& SavedPiece : SavedPieces)
	{
		// Plafond : jamais plus de pièces que la limite monde, quelle que soit la taille du fichier.
		if (SpawnedCount >= EMBBuildingDefaults::MaxPiecesPerWorld)
		{
			SkippedCount += 1;
			continue;
		}

		// Identifiant invalide ou en double : la pièce serait introuvable (ou écraserait l'autre) lors du contrôle des liens.
		if (SavedPiece.SaveId <= 0 || PieceBySaveId.Contains(SavedPiece.SaveId))
		{
			++SkippedCount;
			continue;
		}

		// La ligne de DT_BuildPieces fait foi : classe, matériau et catégorie viennent de la table, JAMAIS du fichier
		// (un fichier édité ne peut donc ni faire charger une classe arbitraire ni changer la solidité d'une pièce).
		const FEMBBuildPieceRow* Row = Table->FindRow<FEMBBuildPieceRow>(SavedPiece.RowName, TEXT("UEMBSaveSubsystem::SpawnSavedPieces"), /*bWarnIfRowMissing*/ false);
		if (!Row || !IsTransformSane(SavedPiece.Transform))
		{
			++SkippedCount;
			continue;
		}

		UClass* PieceClass = nullptr;
		if (UClass** Cached = ClassByRow.Find(SavedPiece.RowName))
		{
			PieceClass = *Cached;
		}
		else
		{
			PieceClass = Row->PieceClass.IsNull() ? nullptr : Row->PieceClass.LoadSynchronous();
			ClassByRow.Add(SavedPiece.RowName, PieceClass);
		}
		if (!PieceClass || !PieceClass->IsChildOf(AEMBBuildablePiece::StaticClass()))
		{
			++SkippedCount;
			continue;
		}

		AEMBBuildablePiece* Piece = World->SpawnActorDeferred<AEMBBuildablePiece>(
			PieceClass, SavedPiece.Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Piece)
		{
			++SkippedCount;
			continue;
		}

		// Ancrage : une fondation l'est toujours ; le mobilier posé sur le terrain l'est s'il l'était à la sauvegarde.
		const bool bAnchored = Row->bIsGroundAnchor || (Row->bRequiresGroundSupport && SavedPiece.bAnchoredToGround);

		// Le PlayerId de session du poseur est inconnu au chargement : seul l'identifiant stable est restauré.
		Piece->InitializePiece(SavedPiece.RowName, Row->Material, Row->Category, bAnchored, INDEX_NONE);
		// Santé corrompue (NaN/infini) : pièce intacte, pas détruite.
		Piece->RestoreFromSave(FMath::IsFinite(SavedPiece.Health) ? SavedPiece.Health : Piece->GetMaxHealth(), SavedPiece.PlacerStableId);
		Piece->FinishSpawning(SavedPiece.Transform);

		++SpawnedCount;
		PieceBySaveId.Add(SavedPiece.SaveId, Piece);
		SaveIdByPiece.Add(Piece, SavedPiece.SaveId);
	}

	// UN recalcul global depuis toutes les ancres. Les pièces non soutenues sont détruites sans effet visible.
	const int32 UnsupportedCount = Structure->EndBulkLoad();

	// Contrôle des liens sauvegardés contre les contacts recalculés : diagnostic seulement, jamais bloquant.
	int32 MismatchCount = 0;
	for (const FEMBPieceSaveData& SavedPiece : SavedPieces)
	{
		AEMBBuildablePiece** Found = PieceBySaveId.Find(SavedPiece.SaveId);
		if (!Found || !IsValid(*Found))
		{
			continue;
		}

		TArray<AEMBBuildablePiece*> Neighbors;
		(*Found)->GetSupportNeighbors(Neighbors);

		TArray<int32> ActualIds;
		for (const AEMBBuildablePiece* Neighbor : Neighbors)
		{
			// Les pièces pré-placées dans le niveau ne sont pas sauvegardées : elles n'ont pas d'identifiant.
			if (const int32* NeighborId = SaveIdByPiece.Find(Neighbor))
			{
				ActualIds.Add(*NeighborId);
			}
		}
		TArray<int32> ExpectedIds = SavedPiece.SupportLinkIds;
		ActualIds.Sort();
		ExpectedIds.Sort();
		if (ActualIds != ExpectedIds)
		{
			++MismatchCount;
		}
	}

	if (SkippedCount > 0)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Chargement des constructions : %d piece(s) ignoree(s) (ligne ou classe inconnue, transform invalide, plafond)."), SkippedCount);
	}
	if (UnsupportedCount > 0)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Chargement des constructions : %d piece(s) non soutenue(s) detruite(s)."), UnsupportedCount);
	}
	if (MismatchCount > 0)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Chargement des constructions : %d piece(s) dont les liens de support recalcules different de ceux de la sauvegarde (non bloquant)."), MismatchCount);
	}
	UE_LOG(LogEmberfall, Log, TEXT("Constructions chargees : %d piece(s) sur %d."), SpawnedCount - UnsupportedCount, SavedPieces.Num());
}

// ---------------------------------------------------------------------------------------------------------------------
// Identités des joueurs
// ---------------------------------------------------------------------------------------------------------------------

bool UEMBSaveSubsystem::ClaimIdentity(APlayerController* PC)
{
	if (ControllerIds.Contains(PC))
	{
		return true; // déjà réclamé : appel répété
	}
	if (UnpersistedControllers.Contains(PC))
	{
		return false;
	}

	const FString StableId = GetStablePlayerId(PC->PlayerState);
	if (StableId.IsEmpty())
	{
		UnpersistedControllers.Add(PC);
		UE_LOG(LogEmberfall, Warning, TEXT("Joueur sans PlayerState exploitable : personnage neuf non persiste."));
		return false;
	}

	// Identifiant déjà porté par un AUTRE joueur connecté (deux invités de même nom, par exemple) : les deux
	// écraseraient mutuellement leurs données. Le second démarre un personnage neuf qui n'est jamais persisté.
	if (const TWeakObjectPtr<APlayerController>* Holder = ActiveIds.Find(StableId))
	{
		if (Holder->IsValid() && Holder->Get() != PC)
		{
			UnpersistedControllers.Add(PC);
			UE_LOG(LogEmberfall, Warning, TEXT("Identifiant de sauvegarde %s deja porte par un autre joueur connecte : personnage neuf non persiste."), *ShortId(StableId));
			return false;
		}
	}

	ActiveIds.Add(StableId, PC);
	ControllerIds.Add(PC, StableId);
	return true;
}

void UEMBSaveSubsystem::ReleaseIdentity(APlayerController* PC)
{
	if (const FString* Id = ControllerIds.Find(PC))
	{
		if (const TWeakObjectPtr<APlayerController>* Holder = ActiveIds.Find(*Id))
		{
			if (Holder->Get() == PC)
			{
				ActiveIds.Remove(*Id);
			}
		}
		ControllerIds.Remove(PC);
	}
	UnpersistedControllers.Remove(PC);
	PendingRestoreControllers.Remove(PC);
	PawnRestoreRetries.RemoveAll([PC](const FPawnRestoreRetry& Retry)
	{
		return Retry.Controller.Get() == PC;
	});
}

FString UEMBSaveSubsystem::GetControllerId(const APlayerController* PC) const
{
	if (!PC)
	{
		return FString();
	}
	if (const FString* Cached = ControllerIds.Find(const_cast<APlayerController*>(PC)))
	{
		return *Cached;
	}
	return GetStablePlayerId(PC->PlayerState);
}

bool UEMBSaveSubsystem::IsRestorePending(const APlayerController* PC) const
{
	return PawnRestoreRetries.ContainsByPredicate([PC](const FPawnRestoreRetry& Retry)
	{
		return Retry.Controller.Get() == PC;
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// Joueurs
// ---------------------------------------------------------------------------------------------------------------------

void UEMBSaveSubsystem::RestorePlayer(APlayerController* PC)
{
	if (!PC || !IsServer())
	{
		return;
	}

	// L'identifiant est réclamé tout de suite (et mis en cache pour la durée de la connexion).
	if (!ClaimIdentity(PC))
	{
		return;
	}

	if (LoadState != ELoadState::Loaded)
	{
		// Chargement en cours (ou pas encore lancé) : on mémorise, FinishLoading rappellera.
		PendingRestoreControllers.AddUnique(PC);
		return;
	}

	RestoreFromCache(PC);
}

void UEMBSaveSubsystem::RestoreFromCache(APlayerController* PC)
{
	const FString StableId = GetControllerId(PC);
	const FEMBPlayerSaveData* Data = PlayerCache.Find(StableId);
	if (!Data)
	{
		UE_LOG(LogEmberfall, Log, TEXT("Joueur %s inconnu de la sauvegarde : personnage neuf."), *ShortId(StableId));
		return;
	}

	ApplyProgression(PC, *Data);

	if (!Data->bHasPawnState)
	{
		return; // seule la progression est connue
	}

	if (!TryRestorePawn(PC, *Data))
	{
		// Le pawn n'existe pas encore (le GameMode le crée après PostLogin) : on retente périodiquement.
		UWorld* World = GetServerWorld();
		FPawnRestoreRetry& Retry = PawnRestoreRetries.AddDefaulted_GetRef();
		Retry.Controller = PC;
		Retry.DeadlineTime = (World ? World->GetTimeSeconds() : 0.0) + static_cast<double>(PawnRestoreTimeoutSeconds);

		if (World)
		{
			FTimerManager& TimerManager = World->GetTimerManager();
			if (!TimerManager.IsTimerActive(RestoreRetryTimerHandle))
			{
				TimerManager.SetTimer(RestoreRetryTimerHandle, this, &UEMBSaveSubsystem::HandleRestoreRetryTimer, PawnRestoreRetryPeriod, /*bLoop*/ true);
			}
		}
	}
}

void UEMBSaveSubsystem::ProcessPendingRestores()
{
	TArray<TWeakObjectPtr<APlayerController>> ToRestore = MoveTemp(PendingRestoreControllers);
	PendingRestoreControllers.Reset();
	for (const TWeakObjectPtr<APlayerController>& Weak : ToRestore)
	{
		if (APlayerController* PC = Weak.Get())
		{
			RestoreFromCache(PC);
		}
	}
}

void UEMBSaveSubsystem::HandleRestoreRetryTimer()
{
	UWorld* World = GetServerWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;

	for (int32 Index = PawnRestoreRetries.Num() - 1; Index >= 0; --Index)
	{
		const FPawnRestoreRetry Retry = PawnRestoreRetries[Index];
		APlayerController* PC = Retry.Controller.Get();
		const FEMBPlayerSaveData* Data = PC ? PlayerCache.Find(GetControllerId(PC)) : nullptr;

		bool bDone = (!PC || !Data);
		if (!bDone && TryRestorePawn(PC, *Data))
		{
			bDone = true;
		}
		else if (!bDone && Now > Retry.DeadlineTime)
		{
			UE_LOG(LogEmberfall, Warning, TEXT("Restauration du pawn du joueur %s abandonnee : aucun pawn apres %.0f s."), *ShortId(GetControllerId(PC)), PawnRestoreTimeoutSeconds);
			bDone = true;
		}

		if (bDone)
		{
			PawnRestoreRetries.RemoveAtSwap(Index);
		}
	}

	if (PawnRestoreRetries.IsEmpty() && World)
	{
		World->GetTimerManager().ClearTimer(RestoreRetryTimerHandle);
	}
}

void UEMBSaveSubsystem::ApplyProgression(APlayerController* PC, const FEMBPlayerSaveData& Data) const
{
	// La progression vit sur le PlayerState, qui existe dès PostLogin : pas besoin d'attendre le pawn.
	AEMBPlayerState* PlayerState = PC ? PC->GetPlayerState<AEMBPlayerState>() : nullptr;
	UEMBProgressionComponent* Progression = PlayerState ? PlayerState->GetProgressionComponent() : nullptr;
	if (Progression)
	{
		Progression->ImportSaveData(Data.Progression);
	}
}

bool UEMBSaveSubsystem::TryRestorePawn(APlayerController* PC, const FEMBPlayerSaveData& Data) const
{
	AEMBPlayerCharacter* Character = PC ? Cast<AEMBPlayerCharacter>(PC->GetPawn()) : nullptr;
	if (!Character)
	{
		return false;
	}

	if (UEMBInventoryComponent* Inventory = Character->GetInventoryComponent())
	{
		Inventory->ImportResources(Data.Resources);
	}
	if (UEMBStatsComponent* Stats = Character->GetStatsComponent())
	{
		if (FMath::IsFinite(Data.Health) && FMath::IsFinite(Data.Stamina))
		{
			Stats->RestoreFromSave(Data.Health, Data.Stamina);
		}
	}

	// Sans transform exploitable (mort sans point sûr connu), le joueur reste au PlayerStart choisi par le GameMode.
	if (Data.bHasPawnTransform && IsTransformSane(Data.PawnTransform))
	{
		const FVector Location = Data.PawnTransform.GetLocation();
		const FRotator Rotation = Data.PawnTransform.Rotator();
		// TeleportTo vérifie la place disponible et peut échouer (collision) : repli par déplacement direct, en
		// téléportation physique (pas de balayage), pour ne jamais laisser le joueur à l'origine.
		if (!Character->TeleportTo(Location, Rotation))
		{
			Character->SetActorLocationAndRotation(Location, Rotation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
		}
		PC->SetControlRotation(Rotation);
	}
	return true;
}

void UEMBSaveSubsystem::BuildPlayerData(APlayerController* PC, FEMBPlayerSaveData& OutData) const
{
	const FString StableId = GetControllerId(PC);

	// On part de l'entrée connue : si le joueur n'a pas de pawn à cet instant, ses données de pawn sont conservées.
	if (const FEMBPlayerSaveData* Existing = PlayerCache.Find(StableId))
	{
		OutData = *Existing;
	}
	OutData.StableId = StableId;
	OutData.LastSeen = FDateTime::UtcNow();

	if (AEMBPlayerState* PlayerState = PC ? PC->GetPlayerState<AEMBPlayerState>() : nullptr)
	{
		if (UEMBProgressionComponent* Progression = PlayerState->GetProgressionComponent())
		{
			Progression->ExportSaveData(OutData.Progression);
		}
	}

	if (const FTransform* SafeTransform = LastSafeTransforms.Find(StableId))
	{
		OutData.bHasLastSafeTransform = true;
		OutData.LastSafeTransform = *SafeTransform;
	}

	// Restauration du pawn encore en attente : le pawn actuel est un personnage NEUF, il ne doit surtout pas écraser
	// l'inventaire, les PV et la position sauvegardés.
	if (IsRestorePending(PC))
	{
		return;
	}

	AEMBPlayerCharacter* Character = PC ? Cast<AEMBPlayerCharacter>(PC->GetPawn()) : nullptr;
	if (!Character)
	{
		return;
	}

	if (const UEMBInventoryComponent* Inventory = Character->GetInventoryComponent())
	{
		Inventory->ExportResources(OutData.Resources);
	}

	const UEMBStatsComponent* Stats = Character->GetStatsComponent();
	const UEMBCombatComponent* Combat = Character->GetCombatComponent();
	const bool bDead = (Stats && Stats->IsDead()) || (Combat && Combat->GetCurrentState() == EEMBCombatState::Dead);

	OutData.bHasPawnState = true;
	if (Stats)
	{
		OutData.Health = Stats->GetHealth();
		OutData.Stamina = Stats->GetStamina();
	}
	OutData.bHasPawnTransform = true;
	OutData.PawnTransform = FTransform(Character->GetActorRotation(), Character->GetActorLocation());

	if (bDead)
	{
		// Un joueur mort est enregistré PV et endurance pleins. Il n'y a aucune pénalité de mort codée.
		if (Stats)
		{
			OutData.Health = Stats->GetMaxHealth();
			OutData.Stamina = Stats->GetMaxStamina();
		}
		if (UEMBProgressionSettings::Get()->DeathSaveBehavior == EEMBDeathSaveBehavior::RespawnAtSafePoint)
		{
			// Dernier point sûr ; à défaut, aucune position : retour au PlayerStart.
			OutData.bHasPawnTransform = OutData.bHasLastSafeTransform;
			OutData.PawnTransform = OutData.LastSafeTransform;
		}
	}
}

void UEMBSaveSubsystem::CaptureAllPlayers()
{
	UWorld* World = GetServerWorld();
	if (!World)
	{
		return;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		// Seuls les joueurs dont l'identité a été réclamée (RestorePlayer appelé) sont capturés : un joueur dont la
		// restauration n'a pas eu lieu est un personnage neuf qui écraserait sa sauvegarde.
		if (!PC || !ControllerIds.Contains(PC) || UnpersistedControllers.Contains(PC))
		{
			continue;
		}
		FEMBPlayerSaveData Data;
		BuildPlayerData(PC, Data);
		if (!Data.StableId.IsEmpty())
		{
			PlayerCache.Add(Data.StableId, Data);
		}
	}
}

void UEMBSaveSubsystem::CapturePlayer(APlayerController* PC)
{
	if (!PC || !IsServer())
	{
		return;
	}

	const bool bLoaded = (LoadState == ELoadState::Loaded);
	if (bLoaded && ControllerIds.Contains(PC) && !UnpersistedControllers.Contains(PC))
	{
		FEMBPlayerSaveData Data;
		BuildPlayerData(PC, Data);
		if (!Data.StableId.IsEmpty())
		{
			PlayerCache.Add(Data.StableId, Data);
		}
	}

	// Le joueur part : son identifiant redevient disponible.
	ReleaseIdentity(PC);

	// Autosave à la déconnexion (reportée si un boss est en rencontre).
	if (bLoaded)
	{
		RequestSave(false);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Minuteries et boss
// ---------------------------------------------------------------------------------------------------------------------

void UEMBSaveSubsystem::HandleAutosaveTimer()
{
	if (IsServer())
	{
		RequestSave(false);
	}
}

void UEMBSaveSubsystem::HandleSafePointTimer()
{
	UWorld* World = GetServerWorld();
	// Tant que le chargement n'est pas terminé, les identités ne sont pas réclamées et les points sûrs sauvegardés
	// ne sont pas encore importés : un relevé maintenant serait écrasé ou orphelin.
	if (!World || !IsServer() || LoadState != ELoadState::Loaded)
	{
		return;
	}

	// Évalué une fois par relevé (pas par joueur).
	const bool bBossActive = IsBossEncounterActive();

	// Dernier point sûr : pawn vivant, AU SOL, état de combat Idle, et hors rencontre de boss (une position prise
	// au milieu de l'arène ramènerait le joueur devant le dragon).
	if (!bBossActive)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const APlayerController* PC = It->Get();
			const AEMBPlayerCharacter* Character = PC ? Cast<AEMBPlayerCharacter>(PC->GetPawn()) : nullptr;
			if (!Character || !ControllerIds.Contains(const_cast<APlayerController*>(PC)))
			{
				continue;
			}
			const UEMBStatsComponent* Stats = Character->GetStatsComponent();
			const UEMBCombatComponent* Combat = Character->GetCombatComponent();
			const UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
			if (Stats && !Stats->IsDead() && Combat && Combat->GetCurrentState() == EEMBCombatState::Idle && Movement && Movement->IsMovingOnGround())
			{
				LastSafeTransforms.Add(GetControllerId(PC), FTransform(Character->GetActorRotation(), Character->GetActorLocation()));
			}
		}
	}

	// Une sauvegarde reportée à cause d'un boss est déclenchée dès que la rencontre est terminée.
	if (bSaveDeferredByBoss && !bBossActive)
	{
		RequestSave(false);
	}
}

void UEMBSaveSubsystem::HandleWorldEventRecorded(FGameplayTag EventTag)
{
	// Boss vaincu : sauvegarde. Si le boss est encore considéré comme actif à cet instant, RequestSave reporte et
	// HandleSafePointTimer finit le travail dans les 10 secondes.
	if (IsServer())
	{
		RequestSave(false);
	}
}

bool UEMBSaveSubsystem::IsBossEncounterActive() const
{
	UWorld* World = GetServerWorld();
	if (!World)
	{
		return false;
	}

	// Rencontre engagée, boss vivant, au moins un joueur vivant à proximité (accesseur public de Dev C).
	// Évalué uniquement au moment de RequestSave et du relevé de 10 s : jamais à chaque frame.
	for (TActorIterator<AEMBDragonBoss> It(World); It; ++It)
	{
		const AEMBDragonBoss* Boss = *It;
		if (Boss && Boss->IsEncounterActive())
		{
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Écriture
// ---------------------------------------------------------------------------------------------------------------------

void UEMBSaveSubsystem::RequestSave(bool bForce)
{
	if (!IsServer())
	{
		return;
	}
	if (LoadState != ELoadState::Loaded)
	{
		// Garde-fou capital : sans chargement terminé, un monde vierge écraserait la vraie sauvegarde.
		UE_LOG(LogEmberfall, Verbose, TEXT("RequestSave ignoree : le chargement du monde n'est pas termine."));
		return;
	}
	if (bBlockedByUnusableSave)
	{
		UE_LOG(LogEmberfall, Verbose, TEXT("RequestSave ignoree : ecritures bloquees (fichier de sauvegarde illisible)."));
		return;
	}
	if (!bForce && IsBossEncounterActive())
	{
		bSaveDeferredByBoss = true;
		return;
	}
	bSaveDeferredByBoss = false;

	if (bWriteInFlight)
	{
		// Une seule écriture en vol : la demande est mise en attente (les demandes multiples fusionnent en une).
		bSavePending = true;
		return;
	}
	StartWrite();
}

void UEMBSaveSubsystem::CapturePieces(TArray<FEMBPieceSaveData>& OutPieces) const
{
	const UWorld* World = GetServerWorld();
	const UEMBStructureSubsystem* Structure = World ? World->GetSubsystem<UEMBStructureSubsystem>() : nullptr;
	if (!Structure)
	{
		return;
	}

	TArray<AEMBBuildablePiece*> AllPieces;
	Structure->GetAllPieces(AllPieces);

	// Seules les pièces posées par des joueurs sont sauvegardées : celles du niveau reviennent avec le niveau.
	TMap<const AEMBBuildablePiece*, int32> SaveIdByPiece;
	TArray<const AEMBBuildablePiece*> Kept;
	Kept.Reserve(AllPieces.Num());
	for (const AEMBBuildablePiece* Piece : AllPieces)
	{
		if (Piece && Piece->WasPlacedByPlayer() && !Piece->IsCollapsed())
		{
			Kept.Add(Piece);
			SaveIdByPiece.Add(Piece, Kept.Num()); // identifiants 1..N
		}
	}

	const AGameStateBase* GameState = World->GetGameState();

	OutPieces.Reserve(Kept.Num());
	for (const AEMBBuildablePiece* Piece : Kept)
	{
		FEMBPieceSaveData& Data = OutPieces.AddDefaulted_GetRef();
		Data.SaveId = SaveIdByPiece[Piece];
		Data.RowName = Piece->GetPieceRowName();
		Data.PieceClassPath = FSoftClassPath(Piece->GetClass()); // informatif : au chargement, la classe vient de la table
		Data.Transform = Piece->GetActorTransform();
		Data.Health = Piece->GetCurrentHealth();
		Data.Material = Piece->GetMaterial();
		Data.Category = Piece->GetCategory();
		Data.bAnchoredToGround = Piece->IsAnchoredToGround();

		// Poseur : identifiant stable déjà connu de la pièce, sinon conversion du PlayerId de session via les joueurs présents.
		Data.PlacerStableId = Piece->GetPlacerStableId();
		if (Data.PlacerStableId.IsEmpty() && Piece->GetPlacerPlayerId() != INDEX_NONE && GameState)
		{
			for (const APlayerState* PlayerState : GameState->PlayerArray)
			{
				if (PlayerState && PlayerState->GetPlayerId() == Piece->GetPlacerPlayerId())
				{
					Data.PlacerStableId = GetStablePlayerId(PlayerState);
					break;
				}
			}
		}

		TArray<AEMBBuildablePiece*> Neighbors;
		Piece->GetSupportNeighbors(Neighbors);
		for (const AEMBBuildablePiece* Neighbor : Neighbors)
		{
			if (const int32* NeighborId = SaveIdByPiece.Find(Neighbor))
			{
				Data.SupportLinkIds.Add(*NeighborId);
			}
		}
	}
}

UEMBSaveGame* UEMBSaveSubsystem::BuildSnapshot()
{
	UEMBSaveGame* Save = Cast<UEMBSaveGame>(UGameplayStatics::CreateSaveGameObject(UEMBSaveGame::StaticClass()));
	if (!Save)
	{
		return nullptr;
	}

	// Instantané complet pris maintenant, sur le thread de jeu ; seule l'écriture disque est asynchrone.
	// COÛT : la capture parcourt toutes les pièces (jusqu'à 15 000) en une frame ; à profiler, à étaler si nécessaire.
	CaptureAllPlayers();

	Save->SaveFormatVersion = UEMBSaveGame::CurrentFormatVersion;
	Save->SaveTimestamp = FDateTime::UtcNow();
	Save->SlotIndex = ActiveSlot;
	PlayerCache.GenerateValueArray(Save->Players);

	// Plafond de 256 joueurs à l'ÉCRITURE (le chargement le borne déjà) : on garde d'abord les joueurs connectés, puis
	// les plus récemment vus. Les exclus sont aussi retirés du cache pour qu'il ne grossisse pas indéfiniment.
	if (Save->Players.Num() > MaxSavedPlayers)
	{
		Save->Players.Sort([this](const FEMBPlayerSaveData& A, const FEMBPlayerSaveData& B)
		{
			const bool bConnectedA = ActiveIds.Contains(A.StableId);
			const bool bConnectedB = ActiveIds.Contains(B.StableId);
			if (bConnectedA != bConnectedB)
			{
				return bConnectedA;
			}
			return A.LastSeen > B.LastSeen;
		});
		Save->Players.SetNum(MaxSavedPlayers);

		TSet<FString> Kept;
		for (const FEMBPlayerSaveData& Player : Save->Players)
		{
			Kept.Add(Player.StableId);
		}
		for (auto It = PlayerCache.CreateIterator(); It; ++It)
		{
			if (!Kept.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
		UE_LOG(LogEmberfall, Warning, TEXT("Sauvegarde : plus de %d joueurs connus, les moins recents sont oublies."), MaxSavedPlayers);
	}

	CapturePieces(Save->Pieces);

	if (const UWorld* World = GetServerWorld())
	{
		if (const UEMBProgressionSubsystem* Progression = World->GetSubsystem<UEMBProgressionSubsystem>())
		{
			Save->World.WorldEvents = Progression->GetWorldEvents();
		}
	}

	// Champs de contrôle de fin de fichier (décomptes + bComplete) : posés en dernier.
	Save->FinalizeForWrite();
	return Save;
}

void UEMBSaveSubsystem::StartWrite()
{
	UEMBSaveGame* Save = BuildSnapshot();
	if (!Save)
	{
		OnSaveCompleted.Broadcast(false);
		return;
	}

	bCurrentWriteToSlotA = bNextWriteToSlotA;
	bWriteInFlight = true;
	InFlightWriteGeneration = LoadGeneration;
	UGameplayStatics::AsyncSaveGameToSlot(Save, BuildSlotName(bCurrentWriteToSlotA), SaveUserIndex,
		FAsyncSaveGameToSlotDelegate::CreateUObject(this, &UEMBSaveSubsystem::OnSlotSaved));
}

void UEMBSaveSubsystem::SaveNowBlocking()
{
	if (!IsServer() || LoadState != ELoadState::Loaded || bBlockedByUnusableSave)
	{
		return;
	}
	if (bWriteInFlight)
	{
		// Une écriture asynchrone vise justement le prochain fichier : une écriture synchrone simultanée dans le même
		// fichier le corromprait. Cette écriture en vol est une sauvegarde récente, on s'en contente.
		// PERTE BORNÉE : seul ce qui s'est passé entre le début de l'écriture asynchrone en vol (prise d'instantané
		// incluse) et l'arrêt du serveur est perdu, soit au plus quelques secondes de jeu. Aucune corruption possible.
		UE_LOG(LogEmberfall, Warning, TEXT("SaveNowBlocking ignoree : une ecriture asynchrone est deja en cours."));
		return;
	}

	UEMBSaveGame* Save = BuildSnapshot();
	if (!Save)
	{
		return;
	}

	bCurrentWriteToSlotA = bNextWriteToSlotA;
	const bool bSuccess = UGameplayStatics::SaveGameToSlot(Save, BuildSlotName(bCurrentWriteToSlotA), SaveUserIndex);
	if (bSuccess)
	{
		bNextWriteToSlotA = !bCurrentWriteToSlotA;
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Echec de l'ecriture synchrone de la sauvegarde (l'autre fichier reste intact)."));
	}
	OnSaveCompleted.Broadcast(bSuccess);
}

void UEMBSaveSubsystem::OnSlotSaved(const FString& SlotName, const int32 UserIndex, bool bSuccess)
{
	bWriteInFlight = false;

	// Écriture lancée pour un monde PRÉCÉDENT (BindToWorld a changé de génération depuis) : l'état A/B et les demandes
	// en attente appartiennent au monde courant et ne doivent pas être modifiés par ce callback tardif.
	if (InFlightWriteGeneration != LoadGeneration)
	{
		UE_LOG(LogEmberfall, Verbose, TEXT("Callback d'ecriture d'un monde precedent ignore (succes=%d)."), bSuccess ? 1 : 0);
		return;
	}

	if (bSuccess)
	{
		// Le fichier qui vient d'être écrit devient le plus récent : la prochaine écriture visera l'autre.
		bNextWriteToSlotA = !bCurrentWriteToSlotA;
	}
	else
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Echec de l'ecriture de la sauvegarde '%s' (l'autre fichier reste intact)."), *SlotName);
	}
	OnSaveCompleted.Broadcast(bSuccess);

	// Demande arrivée pendant l'écriture : un instantané tout neuf part maintenant.
	if (bSavePending)
	{
		bSavePending = false;
		if (IsServer() && LoadState == ELoadState::Loaded && !bBlockedByUnusableSave)
		{
			StartWrite();
		}
	}
}
