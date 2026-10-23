// Emberfall - composant de construction du joueur (Dev B, contrat 3.6).

#include "EMBBuildingComponent.h"

#include "Emberfall.h"
#include "EMBBuildablePiece.h"
#include "EMBCombatComponent.h"
#include "EMBGameplayTags.h"
#include "EMBInventoryComponent.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBSaveSubsystem.h"
#include "EMBStructureSubsystem.h"
#include "Net/UnrealNetwork.h"
#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/OverlapResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameplayTagAssetInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

namespace
{
	/** Tolérance (cm) entre le Z du pivot et la surface détectée. */
	constexpr double GroundSnapTolerance = 25.0;

	/** Tolérance (cm) d'alignement sur la grille ou sur un socket, vérifiée côté serveur. */
	constexpr double GridTolerance = static_cast<double>(EMBBuildingDefaults::GridPositionTolerance);

	/** Arrondit une valeur au multiple de Step le plus proche. */
	double SnapToStep(double Value, double Step)
	{
		return Step > KINDA_SMALL_NUMBER ? FMath::RoundToDouble(Value / Step) * Step : Value;
	}

	/** Socket d'une pièce voisine retenu comme cible d'accrochage. Kind précalculé par la pièce (FName, comparaison O(1)). */
	struct FTargetSocket
	{
		double DistSq = 0.0;
		FVector Location = FVector::ZeroVector;
		FName Kind = NAME_None;
		double PieceYaw = 0.0;
	};
}

UEMBBuildingComponent::UEMBBuildingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// Aucun tick hors mode construction : le composant ne coûte rien au joueur qui ne construit pas.
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// Nécessaire pour que les RPC Server/Client du composant fonctionnent.
	SetIsReplicatedByDefault(true);
}

void UEMBBuildingComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Seul le propriétaire en a besoin (aperçu du coût) : pas de diffusion aux autres joueurs.
	DOREPLIFETIME_CONDITION(UEMBBuildingComponent, BuildCostMultiplier, COND_OwnerOnly);
}

void UEMBBuildingComponent::SetBuildCostMultiplier(float Multiplier)
{
	const AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}
	BuildCostMultiplier = FMath::Clamp(Multiplier, 0.1f, 1.f);
}

TMap<FName, int32> UEMBBuildingComponent::GetEffectiveCost(const FEMBBuildPieceRow& Row) const
{
	TMap<FName, int32> Result;
	Result.Reserve(Row.Cost.Num());
	for (const TPair<FName, int32>& Pair : Row.Cost)
	{
		if (Pair.Value <= 0)
		{
			// Conservé tel quel : HasResources/ConsumeResources refusent les quantités négatives.
			Result.Add(Pair.Key, Pair.Value);
			continue;
		}
		// Arrondi supérieur, minimum 1. La petite marge évite que 10 x 0,9 = 9,0000001 ne donne 10.
		const int32 Scaled = FMath::CeilToInt(static_cast<float>(Pair.Value) * BuildCostMultiplier - 0.001f);
		Result.Add(Pair.Key, FMath::Max(1, Scaled));
	}
	return Result;
}

bool UEMBBuildingComponent::IsPieceUnlocked(FName PieceRowName) const
{
	if (!LoadedTable || PieceRowName.IsNone())
	{
		return true; // table absente : la validation de ligne se fait ailleurs
	}
	const FEMBBuildPieceRow* Row = LoadedTable->FindRow<FEMBBuildPieceRow>(PieceRowName, TEXT("UEMBBuildingComponent::IsPieceUnlocked"), /*bWarnIfRowMissing*/ false);
	if (!Row || Row->RequiredGlyph.IsNone())
	{
		return true; // pas de glyphe requis : toujours disponible
	}

	// Le glyphe se lit dans le composant de progression du PlayerState (répliqué : valable côté serveur ET côté client
	// propriétaire). Sans AEMBPlayerState (mode de test, PlayerState générique), on ne bloque rien : une partie sans
	// progression ne doit pas se retrouver sans pouvoir construire.
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const AEMBPlayerState* EmberfallState = OwnerPawn ? Cast<AEMBPlayerState>(OwnerPawn->GetPlayerState()) : nullptr;
	const UEMBProgressionComponent* Progression = EmberfallState ? EmberfallState->GetProgressionComponent() : nullptr;
	if (!Progression)
	{
		// Un seul avertissement par processus : un PlayerState sans progression laisse TOUT constructible, ce qui est
		// une erreur de configuration (GameMode sans AEMBPlayerState) à voir dans les logs, pas à chaque frame d'aperçu.
		static bool bWarnedNoEmberfallState = false;
		if (!bWarnedNoEmberfallState)
		{
			bWarnedNoEmberfallState = true;
			UE_LOG(LogEmberfall, Warning, TEXT("IsPieceUnlocked : pas de AEMBPlayerState avec composant de progression, les glyphes requis ne sont pas verifies."));
		}
		return true;
	}
	return Progression->HasGlyph(Row->RequiredGlyph);
}

void UEMBBuildingComponent::BeginPlay()
{
	Super::BeginPlay();

	// Serveur ET client propriétaire ont besoin de la table : le serveur pour la validation, le client pour l'aperçu.
	LoadBuildTable();
}

void UEMBBuildingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GhostMesh)
	{
		GhostMesh->DestroyComponent();
		GhostMesh = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------------------------------------------------
// Données
// ---------------------------------------------------------------------------------------------------------------------

void UEMBBuildingComponent::LoadBuildTable()
{
	if (LoadedTable)
	{
		return;
	}
	// Une seule source de vérité pour la table : celle du composant si elle est renseignée, sinon la valeur Config du
	// sous-système de sauvegarde (DefaultGame.ini). La sauvegarde et la construction lisent ainsi forcément la même table.
	const TSoftObjectPtr<UDataTable> TablePath = BuildPiecesTable.IsNull() ? UEMBSaveSubsystem::GetConfiguredBuildPiecesTable() : BuildPiecesTable;
	if (TablePath.IsNull())
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBBuildingComponent: BuildPiecesTable (DT_BuildPieces) n'est renseignee ni sur le composant ni dans la configuration de la sauvegarde."));
		return;
	}
	LoadedTable = TablePath.LoadSynchronous();
}

bool UEMBBuildingComponent::FindRowCopy(FName RowName, FEMBBuildPieceRow& OutRow) const
{
	if (!LoadedTable || RowName.IsNone())
	{
		return false;
	}
	const FEMBBuildPieceRow* Row = LoadedTable->FindRow<FEMBBuildPieceRow>(RowName, TEXT("UEMBBuildingComponent::FindRowCopy"), /*bWarnIfRowMissing*/ false);
	if (!Row)
	{
		return false;
	}
	OutRow = *Row;
	return true;
}

bool UEMBBuildingComponent::IsLocalOwner() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	return Pawn && Pawn->IsLocallyControlled();
}

UClass* UEMBBuildingComponent::ResolvePieceClass(const FEMBBuildPieceRow& Row) const
{
	if (Row.PieceClass.IsNull())
	{
		return nullptr;
	}
	// Chargement synchrone : acceptable à la sélection d'une pièce (client) ou à la pose (serveur, classe déjà
	// chargée après la première pose). À précharger via l'Asset Manager si la table devient très grande.
	UClass* PieceClass = Row.PieceClass.LoadSynchronous();
	return (PieceClass && PieceClass->IsChildOf(AEMBBuildablePiece::StaticClass())) ? PieceClass : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Mode construction (API 3.6)
// ---------------------------------------------------------------------------------------------------------------------

void UEMBBuildingComponent::ToggleBuildMode()
{
	if (bBuildMode)
	{
		CancelBuildMode();
	}
	else
	{
		EnterBuildMode();
	}
}

void UEMBBuildingComponent::EnterBuildMode()
{
	// Le mode construction est purement local : seul le joueur contrôlant ce personnage peut l'activer.
	if (bBuildMode || !IsLocalOwner())
	{
		return;
	}

	LoadBuildTable();
	bBuildMode = true;

	// Tick activé seulement en mode construction, à la fréquence de l'aperçu (30 Hz par défaut).
	// Le moteur espace lui-même les appels : pas de compteur manuel, pas de travail entre deux mises à jour.
	SetComponentTickInterval(1.f / FMath::Max(PreviewUpdateHz, 1.f));
	SetComponentTickEnabled(true);

	// Première entrée : pièce par défaut ; sinon on retrouve la dernière pièce choisie.
	SetSelectedPiece(SelectedRowName.IsNone() ? DefaultPieceRowName : SelectedRowName);

	OnBuildModeChanged.Broadcast(true);
}

void UEMBBuildingComponent::CancelBuildMode()
{
	ExitBuildMode();
}

void UEMBBuildingComponent::ExitBuildMode()
{
	if (!bBuildMode)
	{
		return;
	}

	bBuildMode = false;
	SetComponentTickEnabled(false);

	if (GhostMesh)
	{
		GhostMesh->SetVisibility(false);
	}

	bHasBroadcastResult = false;
	// Résultat d'aperçu périmé : il ne doit jamais pouvoir servir à une pose après la sortie du mode.
	LastEvaluation = FEMBPlacementEvaluation();
	OnBuildModeChanged.Broadcast(false);
}

void UEMBBuildingComponent::SetSelectedPiece(FName PieceRowName)
{
	LoadBuildTable();

	FEMBBuildPieceRow NewRow;
	if (!FindRowCopy(PieceRowName, NewRow))
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBBuildingComponent: ligne '%s' introuvable dans DT_BuildPieces."), *PieceRowName.ToString());
		return;
	}

	SelectedRowName = PieceRowName;
	CachedRow = NewRow;
	bHasCachedRow = true;
	PreviewYaw = 0.0;
	bHasBroadcastResult = false;
	PreviewPieceClass = nullptr;
	bPreviewClassFailed = false;
	LastEvaluation = FEMBPlacementEvaluation();

	if (bBuildMode)
	{
		RefreshPreviewPieceClass();
	}
}

void UEMBBuildingComponent::RotatePreview(float Direction)
{
	if (!bBuildMode || !bHasCachedRow || FMath::IsNearlyZero(Direction))
	{
		return;
	}

	const double Step = static_cast<double>(CachedRow.GetRotationStep());
	PreviewYaw = FRotator::NormalizeAxis(PreviewYaw + (Direction > 0.f ? Step : -Step));

	// Retour immédiat : pas d'attente du prochain tick.
	UpdatePreview();
}

void UEMBBuildingComponent::ConfirmPlacement()
{
	if (!bBuildMode || !bHasCachedRow || !IsLocalOwner())
	{
		return;
	}

	// Anti-rebond : ignore un second clic arrivé dans ConfirmDebounceTime (double-clic, rebond de manette).
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastConfirmTime < static_cast<double>(ConfirmDebounceTime))
	{
		return;
	}
	LastConfirmTime = Now;

	// Évaluation fraîche au moment du clic plutôt que la dernière frame d'aperçu (jusqu'à 33 ms de retard).
	UpdatePreview();

	if (LastEvaluation.Result != EEMBPlacementResult::Valid)
	{
		OnPlacementResult.Broadcast(LastEvaluation.Result, LastEvaluation.PredictedIntegrity);
		return;
	}

	// Le serveur refera TOUTES les vérifications : cette vérification client n'est qu'un confort.
	Server_PlacePiece(SelectedRowName, LastEvaluation.Transform);
}

void UEMBBuildingComponent::RefreshPreviewPieceClass()
{
	if (!bHasCachedRow)
	{
		return;
	}

	PreviewPieceClass = ResolvePieceClass(CachedRow);
	bPreviewClassFailed = (PreviewPieceClass == nullptr);
	if (!PreviewPieceClass)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("UEMBBuildingComponent: classe de piece invalide pour la ligne '%s'."), *SelectedRowName.ToString());
		return;
	}

	EnsureGhost();
	if (!GhostMesh)
	{
		return;
	}

	const AEMBBuildablePiece* PieceCDO = PreviewPieceClass->GetDefaultObject<AEMBBuildablePiece>();
	const UStaticMeshComponent* SourceMesh = PieceCDO ? PieceCDO->GetMeshComponent() : nullptr;
	GhostMesh->SetStaticMesh(SourceMesh ? SourceMesh->GetStaticMesh() : nullptr);

	// Un seul matériau dynamique partagé par tous les emplacements du fantôme.
	if (!GhostMID)
	{
		if (UMaterialInterface* BaseMaterial = GhostMaterial.LoadSynchronous())
		{
			GhostMID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
		}
		else
		{
			UE_LOG(LogEmberfall, Warning, TEXT("UEMBBuildingComponent: GhostMaterial non renseigne, l'apercu n'aura pas de couleur d'etat."));
		}
	}
	if (GhostMID)
	{
		for (int32 SlotIndex = 0; SlotIndex < GhostMesh->GetNumMaterials(); ++SlotIndex)
		{
			GhostMesh->SetMaterial(SlotIndex, GhostMID);
		}
	}
	LastGhostColor = FLinearColor::Transparent;
}

// ---------------------------------------------------------------------------------------------------------------------
// Aperçu local
// ---------------------------------------------------------------------------------------------------------------------

void UEMBBuildingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (bBuildMode)
	{
		UpdatePreview();
	}
}

void UEMBBuildingComponent::EnsureGhost()
{
	if (GhostMesh)
	{
		return;
	}

	AActor* OwnerActor = GetOwner();
	if (!OwnerActor || !OwnerActor->GetRootComponent())
	{
		return;
	}

	GhostMesh = NewObject<UStaticMeshComponent>(OwnerActor, TEXT("EMBBuildGhost"), RF_Transient);
	GhostMesh->SetupAttachment(OwnerActor->GetRootComponent());
	// Transform absolu : le fantôme est positionné en coordonnées monde, il ne suit pas le personnage.
	GhostMesh->SetAbsolute(true, true, true);
	GhostMesh->SetMobility(EComponentMobility::Movable);
	GhostMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GhostMesh->SetGenerateOverlapEvents(false);
	GhostMesh->SetCanEverAffectNavigation(false);
	GhostMesh->SetCastShadow(false);
	GhostMesh->SetIsReplicated(false);
	GhostMesh->SetVisibility(false);
	GhostMesh->RegisterComponent();
}

void UEMBBuildingComponent::UpdatePreview()
{
	if (!bBuildMode || !bHasCachedRow)
	{
		return;
	}

	if (!PreviewPieceClass && !bPreviewClassFailed)
	{
		RefreshPreviewPieceClass();
	}
	const AEMBBuildablePiece* PieceCDO = PreviewPieceClass ? PreviewPieceClass->GetDefaultObject<AEMBBuildablePiece>() : nullptr;
	FTransform Desired;
	if (!PieceCDO || !ComputeDesiredTransform(PieceCDO, Desired))
	{
		// Pas d'aperçu calculable : on invalide l'évaluation pour qu'aucune pose ne parte d'un résultat périmé.
		LastEvaluation = FEMBPlacementEvaluation();
		return;
	}

	if (IsPieceUnlocked(SelectedRowName))
	{
		LastEvaluation = EvaluatePlacement(CachedRow, PieceCDO, SanitizeTransform(CachedRow, Desired), /*bPreview*/ true);
	}
	else
	{
		// Pièce verrouillée (progression) : aperçu rouge, aucune pose possible.
		LastEvaluation = FEMBPlacementEvaluation();
		LastEvaluation.Transform = SanitizeTransform(CachedRow, Desired);
	}
	ApplyGhost(LastEvaluation);

	// On ne notifie l'UI que lorsque le résultat ou l'intégrité entière change (pas 30 fois par seconde).
	const int32 IntegrityBucket = FMath::RoundToInt(LastEvaluation.PredictedIntegrity);
	if (!bHasBroadcastResult || LastBroadcastResult != LastEvaluation.Result || LastBroadcastIntegrity != IntegrityBucket)
	{
		bHasBroadcastResult = true;
		LastBroadcastResult = LastEvaluation.Result;
		LastBroadcastIntegrity = IntegrityBucket;
		OnPlacementResult.Broadcast(LastEvaluation.Result, LastEvaluation.PredictedIntegrity);
	}
}

bool UEMBBuildingComponent::ComputeDesiredTransform(const AEMBBuildablePiece* PieceCDO, FTransform& OutTransform) const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerController* PlayerController = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	UWorld* World = GetWorld();
	if (!Pawn || !PlayerController || !World)
	{
		return false;
	}

	// Tir depuis la caméra (troisième personne) : la longueur couvre la distance caméra-personnage + la portée de pose.
	FVector CameraLocation;
	FRotator CameraRotation;
	PlayerController->GetPlayerViewPoint(CameraLocation, CameraRotation);

	const double TraceLength = FVector::Dist(CameraLocation, Pawn->GetActorLocation()) + MaxPlacementDistance;
	const FVector TraceEnd = CameraLocation + CameraRotation.Vector() * TraceLength;

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBPlacementCursor), /*bTraceComplex*/ false, Pawn);
	const bool bHit = World->LineTraceSingleByChannel(Hit, CameraLocation, TraceEnd, PlacementTraceChannel, Params);
	const FVector CursorPoint = bHit ? FVector(Hit.ImpactPoint) : TraceEnd;

	// 1) Accrochage aux sockets des pièces voisines (rayon 150 cm, socket le plus proche).
	FVector Location = CursorPoint;
	double Yaw = PreviewYaw;
	if (!TrySnapToSockets(CursorPoint, PieceCDO, Location, Yaw))
	{
		// 2) Accrochage à la grille de la catégorie : 300 cm fondation/sol/toit, 150 cm mur/pilier/escalier,
		//    25 cm mobilier/poste (la même règle est vérifiée côté serveur).
		const double Step = GetGridStep(CachedRow);
		Location.X = SnapToStep(CursorPoint.X, Step);
		Location.Y = SnapToStep(CursorPoint.Y, Step);
		Location.Z = CursorPoint.Z;
	}

	OutTransform = FTransform(FRotator(0.0, Yaw, 0.0), Location, FVector::OneVector);
	return true;
}

bool UEMBBuildingComponent::TrySnapToSockets(const FVector& CursorPoint, const AEMBBuildablePiece* PieceCDO, FVector& OutLocation, double& OutYaw) const
{
	const UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this);
	if (!Structure || !PieceCDO)
	{
		return false;
	}

	FEMBSnapSocketArray GhostSockets;
	PieceCDO->GetSnapSocketsLocal(GhostSockets);
	if (GhostSockets.IsEmpty())
	{
		return false;
	}

	TArray<AEMBBuildablePiece*, TInlineAllocator<16>> Nearby;
	{
		TArray<AEMBBuildablePiece*> Found;
		Structure->QueryPiecesInSphere(CursorPoint, SocketSnapRadius, Found);
		Nearby.Append(Found);
	}
	if (Nearby.IsEmpty())
	{
		return false;
	}

	// Sockets des pièces voisines situés dans le rayon d'accrochage autour du curseur.
	const double RadiusSq = FMath::Square(static_cast<double>(SocketSnapRadius));
	TArray<FTargetSocket, TInlineAllocator<16>> Targets;
	FEMBSnapSocketArray WorldSockets;
	for (const AEMBBuildablePiece* Piece : Nearby)
	{
		Piece->GetSnapSocketsWorld(WorldSockets);
		for (const FEMBSnapSocket& Socket : WorldSockets)
		{
			const double DistSq = FVector::DistSquared(Socket.Transform.GetLocation(), CursorPoint);
			if (DistSq <= RadiusSq)
			{
				FTargetSocket& Target = Targets.AddDefaulted_GetRef();
				Target.DistSq = DistSq;
				Target.Location = Socket.Transform.GetLocation();
				Target.Kind = Socket.Kind;
				Target.PieceYaw = Piece->GetActorRotation().Yaw;
			}
		}
	}
	if (Targets.IsEmpty())
	{
		return false;
	}

	// Socket cible le plus proche d'abord.
	Targets.Sort([](const FTargetSocket& A, const FTargetSocket& B)
	{
		return A.DistSq < B.DistSq;
	});

	for (const FTargetSocket& Target : Targets)
	{
		// Le lacet utilisateur est relatif à la pièce visée : sans rotation, la nouvelle pièce s'aligne sur elle.
		const double Yaw = FRotator::NormalizeAxis(Target.PieceYaw + PreviewYaw);
		const FQuat Rotation = FRotator(0.0, Yaw, 0.0).Quaternion();

		// Parmi les sockets compatibles du fantôme, on retient celui dont la position résultante est la plus proche du curseur.
		bool bFound = false;
		double BestScore = TNumericLimits<double>::Max();
		FVector BestLocation = FVector::ZeroVector;
		for (int32 Index = 0; Index < GhostSockets.Num(); ++Index)
		{
			if (GhostSockets[Index].Kind != Target.Kind)
			{
				continue;
			}
			const FVector PivotLocation = Target.Location - Rotation.RotateVector(GhostSockets[Index].Transform.GetLocation());
			const FVector BoxCenter = PivotLocation + Rotation.RotateVector(PieceCDO->GetPlacementBoxCenterOffset());
			const double Score = FVector::DistSquared(BoxCenter, CursorPoint);
			if (Score < BestScore)
			{
				BestScore = Score;
				BestLocation = PivotLocation;
				bFound = true;
			}
		}

		if (bFound)
		{
			OutLocation = BestLocation;
			OutYaw = Yaw;
			return true;
		}
	}

	return false;
}

FLinearColor UEMBBuildingComponent::ComputeGhostColor(const FEMBPlacementEvaluation& Evaluation) const
{
	if (Evaluation.Result != EEMBPlacementResult::Valid)
	{
		return ColorRed;
	}
	// vert > 50 ; jaune 25-50 ; orange 1-24 (section 5).
	if (Evaluation.PredictedIntegrity > EMBBuildingDefaults::PreviewGreenAbove)
	{
		return ColorGreen;
	}
	if (Evaluation.PredictedIntegrity >= EMBBuildingDefaults::PreviewYellowFrom)
	{
		return ColorYellow;
	}
	return ColorOrange;
}

void UEMBBuildingComponent::ApplyGhost(const FEMBPlacementEvaluation& Evaluation)
{
	if (!GhostMesh)
	{
		return;
	}

	GhostMesh->SetWorldTransform(Evaluation.Transform);
	if (!GhostMesh->IsVisible())
	{
		GhostMesh->SetVisibility(true);
	}

	// Le paramètre n'est réécrit que lorsque la couleur change (évite une mise à jour de matériau à chaque tick).
	const FLinearColor Color = ComputeGhostColor(Evaluation);
	if (GhostMID && !Color.Equals(LastGhostColor))
	{
		GhostMID->SetVectorParameterValue(GhostColorParameterName, Color);
		LastGhostColor = Color;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Vérifications (partagées client / serveur)
// ---------------------------------------------------------------------------------------------------------------------

FTransform UEMBBuildingComponent::SanitizeTransform(const FEMBBuildPieceRow& Row, const FTransform& InTransform) const
{
	// Rotation en lacet seul, quantifiée au pas de la pièce : un client ne peut pas envoyer une pièce inclinée.
	const double Step = static_cast<double>(Row.GetRotationStep());
	const double Yaw = FRotator::NormalizeAxis(SnapToStep(InTransform.Rotator().Yaw, Step));
	return FTransform(FRotator(0.0, Yaw, 0.0), InTransform.GetLocation(), FVector::OneVector);
}

bool UEMBBuildingComponent::IsNoBuildZone(AActor* Actor, const UPrimitiveComponent* Component) const
{
	if (!Actor)
	{
		return false;
	}
	if (Actor->ActorHasTag(NoBuildActorTag) || (Component && Component->ComponentHasTag(NoBuildActorTag)))
	{
		return true;
	}
	if (const IGameplayTagAssetInterface* TagOwner = Cast<IGameplayTagAssetInterface>(Actor))
	{
		return TagOwner->HasMatchingGameplayTag(EMBTags::Building_NoBuildZone.GetTag());
	}
	return false;
}

bool UEMBBuildingComponent::IsTerrainActor(const AActor* Actor) const
{
	if (!Actor)
	{
		return false;
	}
	if (Actor->ActorHasTag(TerrainActorTag))
	{
		return true;
	}
	// ALandscapeProxy (et donc ALandscape) détecté par le nom dans la hiérarchie de classes : évite d'ajouter le module
	// Landscape aux dépendances du Build.cs juste pour un IsA.
	static const FName LandscapeProxyName(TEXT("LandscapeProxy"));
	for (const UClass* Class = Actor->GetClass(); Class; Class = Class->GetSuperClass())
	{
		if (Class->GetFName() == LandscapeProxyName)
		{
			return true;
		}
	}
	return false;
}

double UEMBBuildingComponent::GetGridStep(const FEMBBuildPieceRow& Row) const
{
	switch (Row.Category)
	{
	case EEMBPieceCategory::Foundation:
	case EEMBPieceCategory::Floor:
	case EEMBPieceCategory::Roof:
		return static_cast<double>(GridSize);          // 300 cm
	case EEMBPieceCategory::Wall:
	case EEMBPieceCategory::Pillar:
	case EEMBPieceCategory::Stairs:
		return static_cast<double>(GridSize) * 0.5;    // 150 cm
	default:
		return static_cast<double>(FurnitureSnap);     // 25 cm : mobilier, poste de travail
	}
}

bool UEMBBuildingComponent::IsPivotOnSocketCandidate(const AEMBBuildablePiece* PieceCDO, const FTransform& InTransform) const
{
	const UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this);
	if (!Structure || !PieceCDO)
	{
		return false;
	}

	FEMBSnapSocketArray GhostSockets;
	PieceCDO->GetSnapSocketsLocal(GhostSockets);
	if (GhostSockets.IsEmpty())
	{
		return false;
	}

	// Mêmes calculs que l'accrochage de l'aperçu : pivot = socket cible - rotation * socket local du fantôme.
	// Rayon de recherche large (socket cible + demi-étendue de la pièce) pour ne manquer aucun candidat légitime.
	const FVector Pivot = InTransform.GetLocation();
	const FQuat Rotation = InTransform.GetRotation();
	const float SearchRadius = SocketSnapRadius + static_cast<float>(PieceCDO->GetPlacementExtent().GetMax() * 2.0);

	TArray<AEMBBuildablePiece*> Nearby;
	Structure->QueryPiecesInSphere(Pivot, SearchRadius, Nearby);

	FEMBSnapSocketArray WorldSockets;
	for (const AEMBBuildablePiece* Piece : Nearby)
	{
		Piece->GetSnapSocketsWorld(WorldSockets);
		for (const FEMBSnapSocket& Target : WorldSockets)
		{
			for (const FEMBSnapSocket& Ghost : GhostSockets)
			{
				if (Ghost.Kind != Target.Kind)
				{
					continue;
				}
				const FVector Candidate = Target.Transform.GetLocation() - Rotation.RotateVector(Ghost.Transform.GetLocation());
				if (FMath::Abs(Candidate.X - Pivot.X) <= GridTolerance && FMath::Abs(Candidate.Y - Pivot.Y) <= GridTolerance)
				{
					return true;
				}
			}
		}
	}
	return false;
}

UEMBInventoryComponent* UEMBBuildingComponent::GetInventory() const
{
	if (!CachedInventory.IsValid() && GetOwner())
	{
		CachedInventory = GetOwner()->FindComponentByClass<UEMBInventoryComponent>();
	}
	return CachedInventory.Get();
}

EEMBPlacementResult UEMBBuildingComponent::EvaluateGround(const FEMBBuildPieceRow& Row, const AEMBBuildablePiece* PieceCDO, bool bSnapZ, FTransform& InOutTransform, bool& bOutTerrain, TArray<const AActor*>& OutGroundActors) const
{
	UWorld* World = GetWorld();
	if (!World || !PieceCDO)
	{
		return EEMBPlacementResult::InvalidPiece;
	}

	bOutTerrain = false;
	const FVector Pivot = InOutTransform.GetLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBGroundTrace), /*bTraceComplex*/ false, GetOwner());

	if (Row.bIsGroundAnchor)
	{
		// Les pièces déjà posées ne comptent pas comme "sol" pour une fondation.
		if (const UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this))
		{
			TArray<AEMBBuildablePiece*> NearPieces;
			Structure->QueryPiecesInBox(PieceCDO->GetBoundsForTransform(InOutTransform).ExpandBy(GroundTraceDepth), NearPieces);
			for (const AEMBBuildablePiece* NearPiece : NearPieces)
			{
				Params.AddIgnoredActor(NearPiece);
			}
		}

		// 5 tirs verticaux (centre + 4 coins rentrés de 10 %) : coût borné, détecte pente et relief.
		static const FVector2D SamplePattern[5] = { FVector2D(0.0, 0.0), FVector2D(1.0, 1.0), FVector2D(1.0, -1.0), FVector2D(-1.0, 1.0), FVector2D(-1.0, -1.0) };
		const FVector Extent = PieceCDO->GetPlacementExtent();
		const FVector BoxOffset = PieceCDO->GetPlacementBoxCenterOffset();

		double MinZ = TNumericLimits<double>::Max();
		double MaxZ = TNumericLimits<double>::Lowest();
		FVector CenterNormal = FVector::UpVector;

		for (int32 SampleIndex = 0; SampleIndex < 5; ++SampleIndex)
		{
			const FVector LocalPoint(BoxOffset.X + SamplePattern[SampleIndex].X * Extent.X * 0.9, BoxOffset.Y + SamplePattern[SampleIndex].Y * Extent.Y * 0.9, 0.0);
			const FVector Point = InOutTransform.TransformPosition(LocalPoint);
			// Départ légèrement au-dessus de la profondeur autorisée : un terrain plus haut démarre "dans" la matière.
			const FVector Start = Point + FVector(0.0, 0.0, GroundTraceDepth + 10.0);
			const FVector End = Point - FVector(0.0, 0.0, GroundTraceDepth);

			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Start, End, GroundTraceChannel, Params) || !Hit.GetActor())
			{
				return EEMBPlacementResult::NoSupport;
			}
			if (Hit.bStartPenetrating)
			{
				return EEMBPlacementResult::SlopeTooSteep; // relief plus haut que l'enfoncement maximal
			}
			// Une fondation ne s'appuie que sur le TERRAIN (landscape ou acteur marqué Terrain) : ni rocher, ni prop, ni joueur.
			if (!IsTerrainActor(Hit.GetActor()))
			{
				return EEMBPlacementResult::NoSupport;
			}

			MinZ = FMath::Min(MinZ, static_cast<double>(Hit.ImpactPoint.Z));
			MaxZ = FMath::Max(MaxZ, static_cast<double>(Hit.ImpactPoint.Z));
			if (SampleIndex == 0)
			{
				CenterNormal = Hit.ImpactNormal;
			}
			OutGroundActors.AddUnique(Hit.GetActor());
		}

		// Dénivelé sur l'emprise : au-delà de l'enfoncement autorisé, la fondation flotterait ou serait trop enterrée.
		if (MaxZ - MinZ > GroundTraceDepth)
		{
			return EEMBPlacementResult::SlopeTooSteep;
		}

		// Pente maximale 30 degrés, mesurée sur la normale d'impact au centre.
		const double CosMaxSlope = FMath::Cos(FMath::DegreesToRadians(static_cast<double>(MaxFoundationSlopeDeg)));
		if (CenterNormal.Z < CosMaxSlope)
		{
			return EEMBPlacementResult::SlopeTooSteep;
		}

		// Le pivot repose sur le point le plus bas : le reste de la fondation s'enfonce dans le terrain.
		if (bSnapZ)
		{
			FVector Snapped = Pivot;
			Snapped.Z = MinZ;
			InOutTransform.SetLocation(Snapped);
		}
		else if (FMath::Abs(Pivot.Z - MinZ) > GroundSnapTolerance)
		{
			return EEMBPlacementResult::NoSupport;
		}

		bOutTerrain = true;
		return EEMBPlacementResult::Valid;
	}

	// Pièce non porteuse posée sur une surface (mobilier) : un seul tir. Terrain ou pièce comptent comme surface.
	const FVector Start = Pivot + FVector(0.0, 0.0, GroundSnapTolerance * 2.0);
	const FVector End = Pivot - FVector(0.0, 0.0, GroundTraceDepth);
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Start, End, GroundTraceChannel, Params) || !Hit.GetActor() || Hit.bStartPenetrating)
	{
		return EEMBPlacementResult::NoSupport;
	}
	if (Hit.GetActor()->IsA<APawn>())
	{
		return EEMBPlacementResult::NoSupport;
	}

	const double SurfaceZ = Hit.ImpactPoint.Z;
	if (bSnapZ)
	{
		FVector Snapped = Pivot;
		Snapped.Z = SurfaceZ;
		InOutTransform.SetLocation(Snapped);
	}
	else if (FMath::Abs(Pivot.Z - SurfaceZ) > GroundSnapTolerance)
	{
		return EEMBPlacementResult::NoSupport;
	}

	bOutTerrain = IsTerrainActor(Hit.GetActor());
	if (bOutTerrain)
	{
		OutGroundActors.AddUnique(Hit.GetActor());
	}
	return EEMBPlacementResult::Valid;
}

FEMBPlacementEvaluation UEMBBuildingComponent::EvaluatePlacement(const FEMBBuildPieceRow& Row, const AEMBBuildablePiece* PieceCDO, const FTransform& DesiredTransform, bool bPreview) const
{
	FEMBPlacementEvaluation Out;
	Out.Transform = DesiredTransform;

	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor || !PieceCDO)
	{
		Out.Result = EEMBPlacementResult::InvalidPiece;
		return Out;
	}

	// 1) Distance depuis le personnage, mesurée jusqu'à la boîte de la pièce (pas jusqu'au pivot).
	const float DistanceTolerance = bPreview ? 0.f : ServerDistanceTolerance;
	{
		const FBox Bounds = PieceCDO->GetBoundsForTransform(Out.Transform);
		const double Distance = FMath::Sqrt(Bounds.ComputeSquaredDistanceToPoint(OwnerActor->GetActorLocation()));
		if (Distance > static_cast<double>(MaxPlacementDistance) + DistanceTolerance)
		{
			Out.Result = EEMBPlacementResult::TooFar;
			return Out;
		}
	}

	// 2) Alignement (validation serveur) : le pivot doit être sur la grille de SA catégorie (300 / 150 / 25 cm, +-5 cm)
	//    OU coïncider avec une position d'accrochage de socket. Sans cela, un client modifié pourrait poser une pièce à
	//    n'importe quelle position non alignée tant qu'elle touche une voisine.
	if (!bPreview)
	{
		const FVector Location = Out.Transform.GetLocation();
		const double GridStep = GetGridStep(Row);
		const bool bOnGrid = FMath::Abs(Location.X - SnapToStep(Location.X, GridStep)) <= GridTolerance
			&& FMath::Abs(Location.Y - SnapToStep(Location.Y, GridStep)) <= GridTolerance;
		if (!bOnGrid && !IsPivotOnSocketCandidate(PieceCDO, Out.Transform))
		{
			Out.Result = EEMBPlacementResult::InvalidTransform;
			return Out;
		}
	}

	// 3) Appui au sol (fondations) / sur une surface (mobilier). Fournit aussi les acteurs "terrain" à ignorer ensuite.
	bool bTerrainAnchored = false;
	TArray<const AActor*> GroundActors;
	const bool bUsesGround = Row.bIsGroundAnchor || Row.bRequiresGroundSupport;
	if (bUsesGround)
	{
		const EEMBPlacementResult GroundResult = EvaluateGround(Row, PieceCDO, /*bSnapZ*/ bPreview, Out.Transform, bTerrainAnchored, GroundActors);
		if (GroundResult != EEMBPlacementResult::Valid)
		{
			Out.Result = GroundResult;
			return Out;
		}
	}
	Out.bAnchoredToGround = Row.bIsGroundAnchor || bTerrainAnchored;
	const FBox FinalBounds = PieceCDO->GetBoundsForTransform(Out.Transform);

	// 4) Chevauchements et zones interdites. Boîte orientée réduite de 5 % pour tolérer les contacts entre voisines.
	{
		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBPlacementOverlap), /*bTraceComplex*/ false, OwnerActor);
		const FVector BoxCenter = Out.Transform.TransformPosition(PieceCDO->GetPlacementBoxCenterOffset());
		const FCollisionShape Shape = FCollisionShape::MakeBox(PieceCDO->GetPlacementExtent() * static_cast<double>(OverlapBoxShrink));
		World->OverlapMultiByChannel(Overlaps, BoxCenter, Out.Transform.GetRotation(), PlacementOverlapChannel, Shape, Params);

		bool bNoBuildZone = false;
		bool bBlocked = false;
		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* OverlapActor = Overlap.GetActor();
			if (!OverlapActor)
			{
				continue;
			}
			if (IsNoBuildZone(OverlapActor, Overlap.GetComponent()))
			{
				bNoBuildZone = true;
				break;
			}
			// Le terrain sous une fondation (ou un mobilier) n'est pas un obstacle.
			if (bUsesGround && GroundActors.Contains(OverlapActor))
			{
				continue;
			}
			// Une pièce en train de s'effondrer n'occupe plus la place.
			if (const AEMBBuildablePiece* OtherPiece = Cast<AEMBBuildablePiece>(OverlapActor))
			{
				if (OtherPiece->IsCollapsed())
				{
					continue;
				}
			}
			bBlocked = true;
		}

		if (bNoBuildZone)
		{
			Out.Result = EEMBPlacementResult::NoBuildZone;
			return Out;
		}
		if (bBlocked)
		{
			Out.Result = EEMBPlacementResult::Overlapping;
			return Out;
		}
	}

	// 5a) Limites : 15 000 pièces par monde et 3 000 par base (composante connexe, bases fusionnées par la nouvelle pièce comprises).
	if (const UEMBStructureSubsystem* LimitStructure = UEMBStructureSubsystem::Get(this))
	{
		if (LimitStructure->GetNumPieces() >= MaxPiecesPerWorld)
		{
			Out.Result = EEMBPlacementResult::LimitReached;
			return Out;
		}

		FEMBStructureProbe LimitProbe;
		LimitProbe.WorldBounds = FinalBounds;
		// Parcours borné : au plus MaxPiecesPerBase noeuds visités, même pour une base gigantesque.
		const int32 TouchedBasePieces = LimitStructure->CountPiecesInTouchedBases(LimitProbe, MaxPiecesPerBase);
		if (TouchedBasePieces + 1 > MaxPiecesPerBase)
		{
			Out.Result = EEMBPlacementResult::LimitReached;
			return Out;
		}
	}

	// 5b) Intégrité prédite > 0 : la pièce doit toucher des supports et en tirer une intégrité positive.
	if (Out.bAnchoredToGround)
	{
		Out.PredictedIntegrity = EMBBuildingDefaults::GroundedIntegrity;
	}
	else
	{
		const UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this);
		if (!Structure)
		{
			Out.Result = EEMBPlacementResult::NoSupport;
			return Out;
		}

		FEMBStructureProbe Probe;
		Probe.WorldBounds = FinalBounds;
		Probe.Material = Row.Material;
		Probe.bIsPillar = Row.Category == EEMBPieceCategory::Pillar;

		int32 SupportCount = 0;
		Out.PredictedIntegrity = Structure->PredictIntegrity(Probe, &SupportCount);
		if (SupportCount == 0)
		{
			Out.Result = EEMBPlacementResult::NoSupport;
			return Out;
		}
		if (Out.PredictedIntegrity <= EMBBuildingDefaults::CollapseThreshold)
		{
			Out.Result = EEMBPlacementResult::InsufficientIntegrity;
			return Out;
		}
	}

	// 6) Ressources (dernier critère : l'aperçu reste "valide" structurellement tant qu'on ne peut pas payer, mais rouge).
	const UEMBInventoryComponent* Inventory = GetInventory();
	if (!Inventory || !Inventory->HasResources(GetEffectiveCost(Row)))
	{
		Out.Result = EEMBPlacementResult::NotEnoughResources;
		return Out;
	}

	Out.Result = EEMBPlacementResult::Valid;
	return Out;
}

// ---------------------------------------------------------------------------------------------------------------------
// RPC
// ---------------------------------------------------------------------------------------------------------------------

bool UEMBBuildingComponent::Server_PlacePiece_Validate(FName PieceRowName, const FTransform& Transform)
{
	// Ne rejette (donc ne déconnecte) que les données impossibles à produire par le jeu normal.
	if (PieceRowName.IsNone() || Transform.ContainsNaN())
	{
		return false;
	}
	const FVector Location = Transform.GetLocation();
	constexpr double Limit = EMBBuildingDefaults::MaxWorldCoordinate;
	return FMath::Abs(Location.X) < Limit && FMath::Abs(Location.Y) < Limit && FMath::Abs(Location.Z) < Limit;
}

void UEMBBuildingComponent::Server_PlacePiece_Implementation(FName PieceRowName, const FTransform& Transform)
{
	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (!World || !OwnerActor || !OwnerActor->HasAuthority())
	{
		return;
	}

	// Anti-spam : le chronomètre est relancé à CHAQUE requête reçue, rejetée ou non. Un client qui inonde le serveur reste
	// donc bloqué tant qu'il inonde, et n'obtient aucune réponse (silence : pas de bande passante dépensée pour lui).
	const double Now = World->GetTimeSeconds();
	const bool bFlooding = (Now - LastServerRequestTime) < static_cast<double>(MinServerPlaceInterval);
	LastServerRequestTime = Now;
	if (bFlooding)
	{
		return;
	}

	// Un joueur mort ne construit pas. FindComponentByClass : pas de dépendance au personnage concret.
	if (const UEMBCombatComponent* Combat = OwnerActor->FindComponentByClass<UEMBCombatComponent>())
	{
		if (Combat->GetCurrentState() == EEMBCombatState::Dead)
		{
			return;
		}
	}

	// La ligne, la classe et le coût viennent TOUJOURS de la table du serveur, jamais du client.
	LoadBuildTable();
	FEMBBuildPieceRow Row;
	if (!FindRowCopy(PieceRowName, Row) || !IsPieceUnlocked(PieceRowName))
	{
		Client_PlacementResult(EEMBPlacementResult::InvalidPiece, 0.f);
		return;
	}
	UClass* PieceClass = ResolvePieceClass(Row);
	const AEMBBuildablePiece* PieceCDO = PieceClass ? PieceClass->GetDefaultObject<AEMBBuildablePiece>() : nullptr;
	if (!PieceCDO)
	{
		Client_PlacementResult(EEMBPlacementResult::InvalidPiece, 0.f);
		return;
	}

	// Vérification COMPLÈTE : distance (avec tolérance de latence), grille, sol, pente, chevauchement, zones interdites,
	// intégrité et ressources. Mêmes règles que l'aperçu, mais exécutées sur l'état autoritaire du serveur.
	const FTransform SafeTransform = SanitizeTransform(Row, Transform);
	const FEMBPlacementEvaluation Evaluation = EvaluatePlacement(Row, PieceCDO, SafeTransform, /*bPreview*/ false);
	if (Evaluation.Result != EEMBPlacementResult::Valid)
	{
		Client_PlacementResult(Evaluation.Result, Evaluation.PredictedIntegrity);
		return;
	}

	// Consommation atomique : si une ressource manque (course entre deux poses), rien n'est retiré.
	UEMBInventoryComponent* Inventory = GetInventory();
	const TMap<FName, int32> EffectiveCost = GetEffectiveCost(Row);
	if (!Inventory || !Inventory->ConsumeResources(EffectiveCost))
	{
		Client_PlacementResult(EEMBPlacementResult::NotEnoughResources, Evaluation.PredictedIntegrity);
		return;
	}

	// Création différée : l'identité de la pièce est posée AVANT BeginPlay, donc dans le bunch de réplication initial.
	AEMBBuildablePiece* Piece = World->SpawnActorDeferred<AEMBBuildablePiece>(
		PieceClass, Evaluation.Transform, /*Owner*/ nullptr, Cast<APawn>(OwnerActor), ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Piece)
	{
		// Échec technique : remboursement intégral.
		for (const TPair<FName, int32>& Pair : EffectiveCost)
		{
			Inventory->AddResource(Pair.Key, Pair.Value);
		}
		Client_PlacementResult(EEMBPlacementResult::InvalidPiece, 0.f);
		return;
	}

	// Identifiant du poseur conservé sur la pièce (attribution des bases, sauvegarde future).
	int32 PlacerPlayerId = INDEX_NONE;
	if (const APawn* OwnerPawn = Cast<APawn>(OwnerActor))
	{
		if (const APlayerState* PlayerState = OwnerPawn->GetPlayerState())
		{
			PlacerPlayerId = PlayerState->GetPlayerId();
		}
	}
	Piece->InitializePiece(PieceRowName, Row.Material, Row.Category, Evaluation.bAnchoredToGround, PlacerPlayerId);

	// Identifiant stable du poseur (survit aux sessions, contrairement à PlayerId) : stocké pour la sauvegarde.
	if (const APawn* PlacerPawn = Cast<APawn>(OwnerActor))
	{
		Piece->SetPlacerStableId(UEMBSaveSubsystem::GetStablePlayerId(PlacerPawn->GetPlayerState()));
	}

	// FinishSpawning déclenche BeginPlay de la pièce, qui l'enregistre dans UEMBStructureSubsystem :
	// création des liaisons de support, calcul d'intégrité et propagation.
	Piece->FinishSpawning(Evaluation.Transform);

	Client_PlacementResult(EEMBPlacementResult::Valid, Evaluation.PredictedIntegrity);
}

void UEMBBuildingComponent::Client_PlacementResult_Implementation(EEMBPlacementResult Result, float PredictedIntegrity)
{
	OnPlacementResult.Broadcast(Result, PredictedIntegrity);
}
