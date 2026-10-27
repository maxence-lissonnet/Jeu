// Emberfall - points de réapparition et sacs de mort (Dev B, référentiel section 9).

#include "EMBRespawnSubsystem.h"

#include "Emberfall.h"
#include "EMBBuildablePiece.h"
#include "EMBBuildingTypes.h"
#include "EMBDeathBag.h"
#include "EMBSaveSubsystem.h"
#include "EMBStructureSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"

UEMBRespawnSubsystem* UEMBRespawnSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<UEMBRespawnSubsystem>() : nullptr;
}

bool UEMBRespawnSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

bool UEMBRespawnSubsystem::IsAuthority() const
{
	const UWorld* World = GetWorld();
	return World && World->GetNetMode() != NM_Client;
}

FString UEMBRespawnSubsystem::GetPlayerKey(const AController* Controller) const
{
	const APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC || !PC->PlayerState)
	{
		return FString();
	}

	// Source de vérité : le sous-système de sauvegarde, qui a réclamé l'identité du joueur à la connexion
	// (et sait si ce joueur est persisté).
	if (const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		if (const UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			return Save->GetPlayerKey(PC);
		}
	}
	return UEMBSaveSubsystem::GetStablePlayerId(PC->PlayerState);
}

// ---------------------------------------------------------------------------------------------------------------------
// Points de réapparition
// ---------------------------------------------------------------------------------------------------------------------

void UEMBRespawnSubsystem::BindRespawnPoint(APlayerState* Player, AEMBBuildablePiece* Piece)
{
	if (!IsAuthority() || !Player || !IsValid(Piece) || !Piece->IsRespawnPoint() || Piece->IsCollapsed())
	{
		return;
	}

	const FString Key = GetPlayerKey(Cast<AController>(Player->GetOwner()));
	if (Key.IsEmpty())
	{
		return;
	}
	Bindings.Add(Key, Piece); // le dernier lien remplace le précédent
}

AEMBBuildablePiece* UEMBRespawnSubsystem::GetBoundPiece(const FString& PlayerKey) const
{
	const TWeakObjectPtr<AEMBBuildablePiece>* Found = Bindings.Find(PlayerKey);
	AEMBBuildablePiece* Piece = Found ? Found->Get() : nullptr;
	// Pièce détruite (pointeur faible tombé), effondrée, ou qui n'est plus un point de réapparition : liaison invalide.
	return (Piece && !Piece->IsCollapsed() && Piece->IsRespawnPoint()) ? Piece : nullptr;
}

void UEMBRespawnSubsystem::RestoreBinding(const FString& PlayerKey, AEMBBuildablePiece* Piece)
{
	if (IsAuthority() && !PlayerKey.IsEmpty() && IsValid(Piece) && Piece->IsRespawnPoint())
	{
		Bindings.Add(PlayerKey, Piece);
	}
}

bool UEMBRespawnSubsystem::GetRespawnTransform(AController* Controller, FTransform& OutTransform) const
{
	const UWorld* World = GetWorld();
	const AEMBBuildablePiece* Piece = World ? GetBoundPiece(GetPlayerKey(Controller)) : nullptr;
	if (!Piece)
	{
		return false; // aucune liaison valide : PlayerStart
	}

	// 150 cm devant la pièce, posé sur le sol.
	//
	// La trace part de la hauteur du lit/du feu + 100 cm (JAMAIS de plus haut) et descend sur une distance limitée :
	// partir plus haut ferait atterrir le joueur sur le toit d'une maison quand le lit est à l'intérieur. Elle utilise
	// le canal EMBBuilding (ECC_GameTraceChannel2), bloqué par le terrain et par les pièces de construction ; les pièces
	// de toit proches sont ignorées, au cas où un toit bas passerait sous le point de départ.
	const FRotator Facing(0.0, Piece->GetActorRotation().Yaw, 0.0);
	const FVector Point = Piece->GetActorLocation() + Facing.Vector() * EMBBuildingDefaults::RespawnFrontOffset;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBRespawnGround), /*bTraceComplex*/ false, Piece);
	if (const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr)
	{
		Params.AddIgnoredActor(Pawn);
	}
	if (const UEMBStructureSubsystem* Structure = World->GetSubsystem<UEMBStructureSubsystem>())
	{
		TArray<AEMBBuildablePiece*> Nearby;
		Structure->QueryPiecesInBox(FBox(Point - FVector(200.0, 200.0, 500.0), Point + FVector(200.0, 200.0, 500.0)), Nearby);
		for (const AEMBBuildablePiece* Neighbor : Nearby)
		{
			if (Neighbor && Neighbor->GetCategory() == EEMBPieceCategory::Roof)
			{
				Params.AddIgnoredActor(Neighbor);
			}
		}
	}

	FHitResult Hit;
	FVector Location = Piece->GetActorLocation() + Facing.Vector() * EMBBuildingDefaults::RespawnFrontOffset + FVector(0.0, 0.0, 100.0);
	const FVector Start(Point.X, Point.Y, Piece->GetActorLocation().Z + 100.0);
	const FVector End = Start - FVector(0.0, 0.0, 400.0);
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_GameTraceChannel2, Params))
	{
		// Hauteur de capsule non connue ici : on décale de 100 cm, le moteur ajuste l'apparition si besoin.
		Location = FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, 100.0);
	}
	// Sinon : 100 cm au-dessus du pied de la pièce, devant elle (cas d'un sol absent ou hors portée).

	OutTransform = FTransform(Facing, Location);
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sacs de mort
// ---------------------------------------------------------------------------------------------------------------------

FVector UEMBRespawnSubsystem::ResolveBagLocation(const FString& OwnerKey, const FVector& DeathLocation, const AActor* IgnoredActor) const
{
	const UWorld* World = GetWorld();
	if (World)
	{
		// Lieu de la mort projeté au sol : trace de 500 cm vers le bas.
		FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBDeathBagGround), /*bTraceComplex*/ false, IgnoredActor);
		FHitResult Hit;
		const FVector Start = DeathLocation + FVector(0.0, 0.0, 50.0);
		const FVector End = DeathLocation - FVector(0.0, 0.0, EMBBuildingDefaults::DeathBagGroundTraceDepth);
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
		{
			return FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, 20.0);
		}
	}

	// Pas de sol (chute, vide) : dernier point sûr relevé par la sauvegarde.
	if (const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr)
	{
		if (const UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			FTransform Safe;
			if (Save->GetLastSafeTransform(OwnerKey, Safe))
			{
				return Safe.GetLocation();
			}
		}
	}
	return DeathLocation;
}

AEMBDeathBag* UEMBRespawnSubsystem::SpawnBag(const FString& OwnerKey, const FTransform& Transform, int32 OwnerPlayerId, bool bPersistent, const TArray<FEMBResourceStack>& Contents)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	UClass* BagClass = DeathBagClass.IsNull() ? nullptr : DeathBagClass.LoadSynchronous();
	if (!BagClass || !BagClass->IsChildOf(AEMBDeathBag::StaticClass()))
	{
		BagClass = AEMBDeathBag::StaticClass();
	}

	AEMBDeathBag* Bag = World->SpawnActorDeferred<AEMBDeathBag>(BagClass, Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Bag)
	{
		return nullptr;
	}
	Bag->InitializeBag(OwnerKey, OwnerPlayerId, bPersistent, Contents);
	Bag->FinishSpawning(Transform);
	return Bag;
}

void UEMBRespawnSubsystem::DropDeathBag(AController* Controller, const FVector& DeathLocation)
{
	if (!IsAuthority() || !Controller)
	{
		return;
	}
	const FString Key = GetPlayerKey(Controller);
	if (Key.IsEmpty())
	{
		return;
	}

	// Un seul sac actif par joueur : mourir avant d'avoir récupéré le précédent le détruit, contenu compris.
	if (const TWeakObjectPtr<AEMBDeathBag>* Old = BagsByOwner.Find(Key))
	{
		if (AEMBDeathBag* OldBag = Old->Get())
		{
			OldBag->Destroy();
		}
	}
	BagsByOwner.Remove(Key);

	APawn* DeadPawn = Controller->GetPawn();
	UEMBInventoryComponent* Inventory = DeadPawn ? DeadPawn->FindComponentByClass<UEMBInventoryComponent>() : nullptr;
	TArray<FEMBResourceStack> Items;
	if (Inventory)
	{
		Inventory->RemoveAllResources(Items);
	}
	if (Items.IsEmpty())
	{
		return; // rien à déposer : pas de sac vide
	}

	const FVector Location = ResolveBagLocation(Key, DeathLocation, DeadPawn);

	const APlayerController* PC = Cast<APlayerController>(Controller);
	const int32 PlayerId = (PC && PC->PlayerState) ? PC->PlayerState->GetPlayerId() : INDEX_NONE;
	bool bPersistent = true;
	if (const UGameInstance* GameInstance = GetWorld()->GetGameInstance())
	{
		if (const UEMBSaveSubsystem* Save = GameInstance->GetSubsystem<UEMBSaveSubsystem>())
		{
			bPersistent = Save->IsPlayerPersisted(PC);
		}
	}

	if (AEMBDeathBag* Bag = SpawnBag(Key, FTransform(Location), PlayerId, bPersistent, Items))
	{
		BagsByOwner.Add(Key, Bag);
	}
}

void UEMBRespawnSubsystem::RestoreBag(const FString& OwnerKey, const FTransform& Transform, const TArray<FEMBResourceStack>& Contents)
{
	if (!IsAuthority() || OwnerKey.IsEmpty() || Contents.IsEmpty())
	{
		return;
	}
	if (const TWeakObjectPtr<AEMBDeathBag>* Old = BagsByOwner.Find(OwnerKey))
	{
		if (AEMBDeathBag* OldBag = Old->Get())
		{
			OldBag->Destroy();
		}
	}
	BagsByOwner.Remove(OwnerKey);

	// Propriétaire hors ligne au chargement : PlayerId attribué plus tard par NotifyPlayerPresent.
	if (AEMBDeathBag* Bag = SpawnBag(OwnerKey, Transform, INDEX_NONE, /*bPersistent*/ true, Contents))
	{
		BagsByOwner.Add(OwnerKey, Bag);
	}
}

void UEMBRespawnSubsystem::GetPersistentBags(TArray<const AEMBDeathBag*>& OutBags) const
{
	for (const TPair<FString, TWeakObjectPtr<AEMBDeathBag>>& Pair : BagsByOwner)
	{
		const AEMBDeathBag* Bag = Pair.Value.Get();
		if (Bag && Bag->IsPersistent() && !Bag->GetContents().IsEmpty())
		{
			OutBags.Add(Bag);
		}
	}
}

void UEMBRespawnSubsystem::NotifyPlayerPresent(AController* Controller)
{
	const APlayerController* PC = Cast<APlayerController>(Controller);
	if (!IsAuthority() || !PC || !PC->PlayerState)
	{
		return;
	}
	if (const TWeakObjectPtr<AEMBDeathBag>* Found = BagsByOwner.Find(GetPlayerKey(PC)))
	{
		if (AEMBDeathBag* Bag = Found->Get())
		{
			Bag->SetOwnerPlayerId(PC->PlayerState->GetPlayerId());
		}
	}
}

bool UEMBRespawnSubsystem::TryConsumeSleepBenefit(const AController* Controller)
{
	const UWorld* World = GetWorld();
	if (!IsAuthority() || !World)
	{
		return false;
	}
	const FString Key = GetPlayerKey(Controller);
	if (Key.IsEmpty())
	{
		return false;
	}

	const double Now = World->GetTimeSeconds();
	const double* Last = LastSleepBenefitTime.Find(Key);
	if (Last && (Now - *Last) < static_cast<double>(EMBBuildingDefaults::BedSleepCooldownSeconds))
	{
		return false;
	}
	LastSleepBenefitTime.Add(Key, Now);
	return true;
}

void UEMBRespawnSubsystem::NotifyBagDestroyed(AEMBDeathBag* Bag)
{
	if (!Bag)
	{
		return;
	}
	// Retire l'entrée seulement si elle désigne bien CE sac (un nouveau sac peut déjà avoir pris la place).
	const FString& Key = Bag->GetOwnerKey();
	if (const TWeakObjectPtr<AEMBDeathBag>* Found = BagsByOwner.Find(Key))
	{
		if (Found->Get() == Bag || !Found->IsValid())
		{
			BagsByOwner.Remove(Key);
		}
	}
}
