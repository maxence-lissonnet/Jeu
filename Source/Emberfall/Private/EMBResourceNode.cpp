// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBResourceNode.h"

#include "Emberfall.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBEquipmentComponent.h"
#include "EMBFallingTree.h"
#include "EMBInventoryComponent.h"
#include "EMBItemTypes.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBProgressionTypes.h"
#include "EMBResourceNodeSubsystem.h"

#include "Components/BoxComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsEngine/BodySetup.h"
#include "TimerManager.h"

namespace
{
	/** Canal de coup de mêlée des joueurs : EMBMeleeHit = ECC_GameTraceChannel1 (référentiel §4.3). */
	constexpr ECollisionChannel EMBMeleeHitChannel = ECC_GameTraceChannel1;

	/** Valeurs de NetState (0 n'est jamais émis). */
	constexpr uint8 NetStateAvailable = 1;
	constexpr uint8 NetStateDepleted = 2;
	constexpr uint8 NetStateNightHidden = 3;
	constexpr uint8 NetStateDepletedAndHidden = 4;

	/** Clé d'attribution d'un récolteur : son PlayerState (survit à la mort du pawn), à défaut le pawn. */
	AActor* GetOwnerKey(APawn& Harvester)
	{
		if (APlayerState* PlayerState = Harvester.GetPlayerState())
		{
			return PlayerState;
		}
		return &Harvester;
	}
}

AEMBResourceNode::AEMBResourceNode()
{
	// Aucun Tick : le nœud réagit aux coups et aux changements d'environnement.
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false);
	// Dormant au repos : seul un changement d'état (FlushNetDormancy + ForceNetUpdate) coûte de la réplication.
	NetDormancy = DORM_Initial;
	SetNetUpdateFrequency(1.f);
	SetMinNetUpdateFrequency(1.f);
	SetNetCullDistanceSquared(FMath::Square(EMBHarvestDefaults::NodeNetCullDistance));

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);

	// Block Visibility / WorldStatic (et Pawn, WorldDynamic, PhysicsBody pour arrêter les déplacements : une réponse Block
	// n'est effective que si les deux composants se bloquent mutuellement) ; Overlap sur EMBMeleeHit pour la lame des joueurs.
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Mesh->SetCollisionObjectType(ECC_WorldStatic);
	Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	Mesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Mesh->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	Mesh->SetCollisionResponseToChannel(EMBMeleeHitChannel, ECR_Overlap);
}

void AEMBResourceNode::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBResourceNode, NetState);
	// La ligne ne change jamais après la création : une seule fois suffit (nœuds dynamiques).
	DOREPLIFETIME_CONDITION(AEMBResourceNode, RowName, COND_InitialOnly);
}

void AEMBResourceNode::PostActorCreated()
{
	Super::PostActorCreated();

	// Nœud posé dans l'éditeur : identifiant stable dès la création (jamais en jeu).
	if (!bDynamicNode && !NodeId.IsValid())
	{
		const UWorld* World = GetWorld();
		if (World && !World->IsGameWorld())
		{
			Modify();
			NodeId = FGuid::NewGuid();
		}
	}
}

void AEMBResourceNode::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	// Filet pour les nœuds posés avant l'ajout de NodeId : généré dans l'éditeur uniquement, et marqué modifié pour
	// que le niveau soit enregistré avec la valeur (sinon l'identifiant changerait à chaque ouverture).
	if (!bDynamicNode && !NodeId.IsValid())
	{
		const UWorld* World = GetWorld();
		if (World && !World->IsGameWorld())
		{
			Modify();
			NodeId = FGuid::NewGuid();
		}
	}
}

#if WITH_EDITOR
void AEMBResourceNode::PostEditImport()
{
	Super::PostEditImport();
	// Copie : un nouvel identifiant, sinon la copie partagerait l'état de réapparition de l'original.
	if (!bDynamicNode)
	{
		NodeId = FGuid::NewGuid();
	}
}
#endif

void AEMBResourceNode::BeginPlay()
{
	Super::BeginPlay();

	// Collision de repli + mémorisation de l'état d'origine des composants (aussi sur les clients).
	CacheComponentStates();

	if (!HasAuthority())
	{
		// Client : applique l'état répliqué déjà reçu (épuisé / caché).
		ApplyAvailabilityState();
		return;
	}

	if (!bDynamicNode && !NodeId.IsValid())
	{
		// Niveau enregistré avant l'ajout de NodeId : la réapparition fonctionne mais ne sera pas sauvegardée.
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : %s n'a pas de NodeId (ouvrir et enregistrer le niveau). Identifiant temporaire utilisé."), *GetNameSafe(this));
		NodeId = FGuid::NewGuid();
	}

	if (UEMBResourceNodeSubsystem* Subsystem = UEMBResourceNodeSubsystem::Get(this))
	{
		// Charge la table au besoin et applique l'état en attente (sauvegarde chargée, streaming).
		Subsystem->RegisterNode(this);
	}
	// Premier état répliqué (1 = disponible, ou épuisé si restauré) : NetState vaut 0 tant qu'il n'est pas émis.
	PushNetState();

	const FEMBResourceNodeRow* Row = FindRow();
	if (!Row)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : ligne '%s' introuvable pour %s, le nœud est inactif."), *RowName.ToString(), *GetNameSafe(this));
		return;
	}
	if (!bDepleted)
	{
		Health = Row->MaxHealth;
	}

	if (Row->bNightOnly)
	{
		// Bascule jour/nuit : abonnement à l'environnement (aucun timer par filon à 10 s). Un contrôle de repli peu
		// fréquent, à phase aléatoire, couvre un setter d'heure qui ne diffuserait pas.
		if (UEMBEnvironmentSubsystem* Environment = GetWorld()->GetSubsystem<UEMBEnvironmentSubsystem>())
		{
			Environment->OnEnvironmentChanged.AddDynamic(this, &AEMBResourceNode::HandleEnvironmentChanged);
		}
		GetWorldTimerManager().SetTimer(NightTimerHandle, this, &AEMBResourceNode::RefreshNightState,
			EMBHarvestDefaults::NightFallbackRefreshSeconds, true,
			FMath::FRandRange(0.f, EMBHarvestDefaults::NightFallbackRefreshSeconds));
		RefreshNightState();
	}
}

void AEMBResourceNode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(NightTimerHandle);
		World->GetTimerManager().ClearTimer(FellingTimerHandle);
		if (UEMBEnvironmentSubsystem* Environment = World->GetSubsystem<UEMBEnvironmentSubsystem>())
		{
			Environment->OnEnvironmentChanged.RemoveDynamic(this, &AEMBResourceNode::HandleEnvironmentChanged);
		}
		if (HasAuthority())
		{
			if (UEMBResourceNodeSubsystem* Subsystem = World->GetSubsystem<UEMBResourceNodeSubsystem>())
			{
				Subsystem->UnregisterNode(this);
			}
		}
	}
	if (FractureLoadHandle.IsValid())
	{
		FractureLoadHandle->CancelHandle();
		FractureLoadHandle.Reset();
	}
	if (FallingLoadHandle.IsValid())
	{
		FallingLoadHandle->CancelHandle();
		FallingLoadHandle.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AEMBResourceNode::InitDynamicNode(const FName InRowName)
{
	RowName = InRowName;
	bDynamicNode = true;
	// Courte vie, pas de milliers d'instances : pas de dormance (un acteur DORM_Initial créé à l'exécution n'est pas un
	// acteur de niveau).
	NetDormancy = DORM_Awake;
}

const FEMBResourceNodeRow* AEMBResourceNode::FindRow() const
{
	const UWorld* World = GetWorld();
	const UEMBResourceNodeSubsystem* Subsystem = World ? World->GetSubsystem<UEMBResourceNodeSubsystem>() : nullptr;
	return Subsystem ? Subsystem->FindRow(RowName) : nullptr;
}

bool AEMBResourceNode::IsWithinHarvestRange(const APawn& Harvester) const
{
	const FVector PawnLocation = Harvester.GetActorLocation();
	// Distance au volume englobant du mesh (un grand arbre ou un rocher a son origine loin de sa surface) ou à
	// l'origine si elle est plus proche : choix assumé, plus juste que l'origine seule pour les gros nœuds.
	float DistanceSquared = FVector::DistSquared(PawnLocation, GetActorLocation());
	if (Mesh)
	{
		DistanceSquared = FMath::Min(DistanceSquared, Mesh->Bounds.GetBox().ComputeSquaredDistanceToPoint(PawnLocation));
	}
	return DistanceSquared <= FMath::Square(EMBHarvestDefaults::MaxHarvestDistance);
}

// ----------------------------------------------------------------------------------------------
// Contrôle d'un coup
// ----------------------------------------------------------------------------------------------

EEMBHarvestReject AEMBResourceNode::ResolveHarvest(const APawn* Harvester, const FEMBResourceNodeRow& Row, FResolvedHarvest& Out) const
{
	// Instigateur : pawn contrôlé par un joueur, à portée.
	if (!Harvester || !Harvester->IsPlayerControlled())
	{
		return EEMBHarvestReject::Unavailable;
	}
	if (!IsWithinHarvestRange(*Harvester))
	{
		return EEMBHarvestReject::TooFar;
	}

	// Outil en main droite ; sans composant d'équipement ou outil cassé : mains nues.
	const UEMBEquipmentComponent* Equipment = Harvester->FindComponentByClass<UEMBEquipmentComponent>();
	FEMBEquippedToolInfo Tool; // défauts : None, tier 0, 5 dégâts
	if (Equipment)
	{
		Tool = Equipment->GetEquippedTool();
	}
	const bool bToolUsable = !Tool.bBroken;
	const EEMBToolType ToolType = bToolUsable ? Tool.ToolType : EEMBToolType::None;
	const float Damage = bToolUsable ? Tool.HarvestDamage : EMBItemDefaults::BareHandsHarvestDamage;

	// Type exigé : correspondance exacte. Aucun outil exigé (buisson, silex, eau) : tout outil convient, mais seul un
	// outil du bon type donne un tier (rendement x1,0 sinon).
	const bool bNeedsTool = Row.RequiredToolType != EEMBToolType::None;
	if (bNeedsTool && ToolType != Row.RequiredToolType)
	{
		return EEMBHarvestReject::WrongTool;
	}
	const int32 Tier = bNeedsTool ? Tool.Tier : 0;
	if (Tier < Row.MinToolTier)
	{
		return EEMBHarvestReject::TierTooLow;
	}
	if (Damage <= 0.f)
	{
		return EEMBHarvestReject::WrongTool;
	}

	// Rendement : tier x passif HarvestYield. Entre MinToolTier et FullYieldTier : x0,3.
	const float TierMultiplier = (Tier < Row.FullYieldTier)
		? EMBHarvestDefaults::UnderTierYieldMultiplier
		: EMBHarvestDefaults::GetTierYieldMultiplier(Tier);
	float PassiveMultiplier = 1.f;
	if (const AEMBPlayerState* PlayerState = Harvester->GetPlayerState<AEMBPlayerState>())
	{
		if (const UEMBProgressionComponent* Progression = PlayerState->GetProgressionComponent())
		{
			PassiveMultiplier += Progression->GetPassiveModifiers().HarvestYield;
		}
	}

	Out.Tier = Tier;
	Out.Damage = Damage;
	Out.Multiplier = TierMultiplier * FMath::Max(0.f, PassiveMultiplier);
	return EEMBHarvestReject::None;
}

EEMBHarvestReject AEMBResourceNode::GetRejectReason(const APawn* Harvester) const
{
	if (bDepleted || bFelling)
	{
		return EEMBHarvestReject::Unavailable;
	}
	if (bHiddenByNight)
	{
		return EEMBHarvestReject::NightOnly;
	}

	// Côté client la table n'est pas chargée d'office : on la charge au premier besoin (quelques lignes).
	const UWorld* World = GetWorld();
	UEMBResourceNodeSubsystem* Subsystem = World ? World->GetSubsystem<UEMBResourceNodeSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return EEMBHarvestReject::Unknown;
	}
	Subsystem->EnsureTableLoaded();
	const FEMBResourceNodeRow* Row = Subsystem->FindRow(RowName);
	if (!Row)
	{
		return EEMBHarvestReject::Unknown;
	}
	FResolvedHarvest Resolved;
	return ResolveHarvest(Harvester, *Row, Resolved);
}

// ----------------------------------------------------------------------------------------------
// Coup reçu
// ----------------------------------------------------------------------------------------------

EEMBHitResult AEMBResourceNode::ReceiveEMBHit(const FEMBHitData& Hit)
{
	if (!HasAuthority() || bDepleted || bFelling || bHiddenByNight)
	{
		return EEMBHitResult::Ignored;
	}
	const FEMBResourceNodeRow* Row = FindRow();
	if (!Row)
	{
		return EEMBHitResult::Ignored;
	}

	APawn* Harvester = Cast<APawn>(Hit.Instigator.Get());
	FResolvedHarvest Resolved;
	if (ResolveHarvest(Harvester, *Row, Resolved) != EEMBHarvestReject::None)
	{
		return EEMBHitResult::Ignored;
	}

	// Dégâts et part du rendement correspondante. Source inépuisable : chaque coup verse le rendement de base entier.
	float Fraction = 1.f;
	bool bNowDepleted = false;
	if (!Row->bInexhaustible)
	{
		if (Health <= 0.f)
		{
			Health = Row->MaxHealth; // table chargée après le BeginPlay du nœud
		}
		const float AppliedDamage = FMath::Min(Resolved.Damage, Health);
		Health -= AppliedDamage;
		Fraction = (Row->MaxHealth > 0.f) ? AppliedDamage / Row->MaxHealth : 1.f;
		bNowDepleted = Health <= KINDA_SMALL_NUMBER;
	}

	AccumulateYield(*Harvester, *Row, Fraction, Resolved.Multiplier);

	// Durabilité : volontairement NON gérée ici. Ce nœud renvoie Hit/Killed, le combat émet alors OnHitLandedServer et
	// UEMBEquipmentComponent retire 1 de durabilité une seule fois (arbitrage du chef). Appeler NotifyToolUsed()
	// en plus doublerait l'usure.

	FlushNetDormancy(); // le multicast d'un acteur dormant n'est pas garanti sans réveil
	Multicast_PlayHitFeedback(FVector_NetQuantize(Hit.HitLocation));

	if (bNowDepleted)
	{
		// Direction du coup : la lame vers le nœud ; à défaut, du joueur vers le nœud.
		FVector HitDirection = Hit.HitDirection.GetSafeNormal2D();
		if (HitDirection.IsNearlyZero())
		{
			HitDirection = (GetActorLocation() - Harvester->GetActorLocation()).GetSafeNormal2D();
		}
		BeginDepletion(*Harvester, *Row, HitDirection);
		return EEMBHitResult::Killed;
	}
	return EEMBHitResult::Hit;
}

void AEMBResourceNode::AccumulateYield(APawn& Harvester, const FEMBResourceNodeRow& Row, const float Fraction, const float Multiplier)
{
	AActor* OwnerKey = GetOwnerKey(Harvester);
	for (const FEMBResourceYield& Yield : Row.Yields)
	{
		if (Yield.ResourceId.IsNone() || Yield.Amount <= 0)
		{
			continue;
		}

		FHarvestAccumulator* Accumulator = Accumulators.FindByPredicate([OwnerKey, &Yield](const FHarvestAccumulator& Candidate)
		{
			return Candidate.Owner.Get() == OwnerKey && Candidate.ResourceId == Yield.ResourceId;
		});
		if (!Accumulator)
		{
			FHarvestAccumulator& Added = Accumulators.AddDefaulted_GetRef();
			Added.Owner = OwnerKey;
			Added.ResourceId = Yield.ResourceId;
			Accumulator = &Added;
		}

		Accumulator->Total += static_cast<float>(Yield.Amount) * Multiplier * Fraction;

		// Arrondi à l'unité inférieure par coup (petite tolérance contre l'erreur de virgule flottante).
		const int32 Whole = FMath::FloorToInt(Accumulator->Total + 0.001f) - Accumulator->Paid;
		if (Whole > 0 && PayResource(OwnerKey, Yield.ResourceId, Whole))
		{
			Accumulator->Paid += Whole;
		}
	}
}

void AEMBResourceNode::FlushAccumulators(AActor* DepleterKey)
{
	// Reste = arrondi du total de la ressource (tous récolteurs confondus) moins ce qui a déjà été versé : la somme
	// versée retombe sur le total, sans surplus quand plusieurs joueurs se partagent le nœud. Il va au joueur qui épuise.
	TMap<FName, float> Totals;
	TMap<FName, int32> PaidTotals;
	for (const FHarvestAccumulator& Accumulator : Accumulators)
	{
		Totals.FindOrAdd(Accumulator.ResourceId) += Accumulator.Total;
		PaidTotals.FindOrAdd(Accumulator.ResourceId) += Accumulator.Paid;
	}
	for (const TPair<FName, float>& Pair : Totals)
	{
		const int32 Remainder = FMath::RoundToInt(Pair.Value) - PaidTotals.FindRef(Pair.Key);
		if (Remainder > 0)
		{
			PayResource(DepleterKey, Pair.Key, Remainder);
		}
	}
	Accumulators.Reset();
}

bool AEMBResourceNode::PayResource(AActor* Owner, const FName ResourceId, const int32 Amount)
{
	APawn* Pawn = nullptr;
	if (const APlayerState* PlayerState = Cast<APlayerState>(Owner))
	{
		Pawn = PlayerState->GetPawn();
	}
	else
	{
		Pawn = Cast<APawn>(Owner);
	}
	UEMBInventoryComponent* Inventory = Pawn ? Pawn->FindComponentByClass<UEMBInventoryComponent>() : nullptr;
	if (!Inventory)
	{
		return false;
	}
	Inventory->AddResource(ResourceId, Amount);
	return true;
}

// ----------------------------------------------------------------------------------------------
// Épuisement
// ----------------------------------------------------------------------------------------------

void AEMBResourceNode::BeginDepletion(APawn& Harvester, const FEMBResourceNodeRow& Row, const FVector& HitDirection)
{
	FlushAccumulators(GetOwnerKey(Harvester));

	// XP versée au joueur qui épuise le nœud.
	if (Row.XPOnDepleted > 0)
	{
		if (const AEMBPlayerState* PlayerState = Harvester.GetPlayerState<AEMBPlayerState>())
		{
			if (UEMBProgressionComponent* Progression = PlayerState->GetProgressionComponent())
			{
				Progression->AddXP(Row.XPOnDepleted);
			}
		}
	}

	switch (Row.Kind)
	{
	case EEMBResourceNodeKind::Tree:
	{
		// Craquement cosmétique, puis chute (FinishFelling). Pendant ce délai, le nœud ne reçoit plus de coups.
		bFelling = true;
		FellingDirection = -HitDirection; // l'arbre tombe vers le bûcheron
		FellingOwnerKey = GetOwnerKey(Harvester);
		PendingFallingClass = Row.FallingClass;
		if (!PendingFallingClass.IsNull())
		{
			// Chargement asynchrone pendant le craquement : pas de hitch au moment de la chute.
			FallingLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(PendingFallingClass.ToSoftObjectPath());
		}
		FlushNetDormancy();
		Multicast_PlayFellingCrack(EMBHarvestDefaults::FellingCrackSeconds);
		GetWorldTimerManager().SetTimer(FellingTimerHandle, this, &AEMBResourceNode::FinishFelling,
			EMBHarvestDefaults::FellingCrackSeconds, false);
		break;
	}
	case EEMBResourceNodeKind::Rock:
	case EEMBResourceNodeKind::Vein:
		if (!Row.FractureClass.IsNull())
		{
			FlushNetDormancy();
			Multicast_SpawnFracture(Row.FractureClass);
		}
		ConcludeDepletion();
		break;
	default:
		ConcludeDepletion();
		break;
	}
}

void AEMBResourceNode::FinishFelling()
{
	bFelling = false;

	UStaticMesh* TreeMesh = Mesh ? Mesh->GetStaticMesh() : nullptr;
	UEMBResourceNodeSubsystem* Subsystem = UEMBResourceNodeSubsystem::Get(this);
	if (Subsystem && TreeMesh)
	{
		// Classe personnalisée si elle a eu le temps de se charger, sinon la classe de base. Si le plafond de chutes est
		// atteint, SpawnFallingTree retourne nullptr et l'arbre disparaît simplement.
		TSubclassOf<AEMBFallingTree> TreeClass = AEMBFallingTree::StaticClass();
		if (UClass* LoadedClass = PendingFallingClass.Get())
		{
			if (LoadedClass->IsChildOf(AEMBFallingTree::StaticClass()))
			{
				TreeClass = LoadedClass;
			}
		}
		Subsystem->SpawnFallingTree(TreeClass, TreeMesh, GetActorTransform(), FellingDirection, FellingOwnerKey.Get());
	}

	FallingLoadHandle.Reset();
	PendingFallingClass.Reset();
	ConcludeDepletion();
}

void AEMBResourceNode::ConcludeDepletion()
{
	SetDepletedState(true);

	if (bDynamicNode)
	{
		// Carcasse consommée : disparition courte, jamais de réapparition.
		SetLifeSpan(EMBHarvestDefaults::DynamicDespawnDelaySeconds);
		return;
	}

	const FEMBResourceNodeRow* Row = FindRow();
	if (Row && Row->RespawnSeconds > 0.f)
	{
		if (UEMBResourceNodeSubsystem* Subsystem = UEMBResourceNodeSubsystem::Get(this))
		{
			Subsystem->ScheduleRespawn(this, Row->RespawnSeconds);
		}
	}
}

void AEMBResourceNode::Respawn()
{
	if (!HasAuthority())
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(FellingTimerHandle);
	bFelling = false;
	Accumulators.Reset();
	if (const FEMBResourceNodeRow* Row = FindRow())
	{
		Health = Row->MaxHealth;
	}
	SetDepletedState(false);
}

void AEMBResourceNode::RestoreDepleted()
{
	if (!HasAuthority())
	{
		return;
	}
	GetWorldTimerManager().ClearTimer(FellingTimerHandle);
	bFelling = false;
	Accumulators.Reset();
	Health = 0.f;
	SetDepletedState(true);
}

// ----------------------------------------------------------------------------------------------
// État répliqué, collision et mise en scène
// ----------------------------------------------------------------------------------------------

void AEMBResourceNode::SetDepletedState(const bool bNewDepleted)
{
	if (bDepleted == bNewDepleted)
	{
		return;
	}
	bDepleted = bNewDepleted;
	PushNetState();
}

void AEMBResourceNode::PushNetState()
{
	const uint8 NewState = bDepleted
		? (bHiddenByNight ? NetStateDepletedAndHidden : NetStateDepleted)
		: (bHiddenByNight ? NetStateNightHidden : NetStateAvailable);
	if (NewState != NetState)
	{
		FlushNetDormancy(); // réveille la réplication pour ce changement, puis retour au sommeil
		NetState = NewState;
		ForceNetUpdate();
	}
	ApplyAvailabilityState();
}

void AEMBResourceNode::OnRep_NetState()
{
	bDepleted = (NetState == NetStateDepleted || NetState == NetStateDepletedAndHidden);
	bHiddenByNight = (NetState == NetStateNightHidden || NetState == NetStateDepletedAndHidden);
	ApplyAvailabilityState();
}

void AEMBResourceNode::CacheComponentStates()
{
	if (bComponentsCached)
	{
		return;
	}
	bComponentsCached = true;

	// Avant la mémorisation : la boîte de repli doit en faire partie.
	EnsureHitCollision();

	TInlineComponentArray<UPrimitiveComponent*> Primitives;
	GetComponents(Primitives);
	ComponentStates.Reserve(Primitives.Num());
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		FComponentState& State = ComponentStates.AddDefaulted_GetRef();
		State.Component = Primitive;
		State.Collision = Primitive->GetCollisionEnabled();
		State.bHiddenInGame = Primitive->bHiddenInGame;
	}
}

void AEMBResourceNode::EnsureHitCollision()
{
	// Un composant qui peut recevoir le balayage de la lame (réponse non Ignore sur EMBMeleeHit) ET porte une forme
	// de collision suffit ; sinon (carcasse sans mesh ou sans collision) on ajoute une boîte de repli.
	TInlineComponentArray<UPrimitiveComponent*> Primitives;
	GetComponents(Primitives);
	for (UPrimitiveComponent* Primitive : Primitives)
	{
		if (!Primitive->IsCollisionEnabled() || Primitive->GetCollisionResponseToChannel(EMBMeleeHitChannel) == ECR_Ignore)
		{
			continue;
		}
		const UBodySetup* BodySetup = Primitive->GetBodySetup();
		if (BodySetup && (BodySetup->AggGeom.GetElementCount() > 0 || BodySetup->CollisionTraceFlag == CTF_UseComplexAsSimple))
		{
			return;
		}
	}

	UBoxComponent* FallbackBox = NewObject<UBoxComponent>(this, TEXT("FallbackHitBox"));
	const float HalfExtent = EMBHarvestDefaults::FallbackHitBoxHalfExtent;
	FallbackBox->SetBoxExtent(FVector(HalfExtent));
	FallbackBox->SetRelativeLocation(FVector(0.f, 0.f, HalfExtent));
	FallbackBox->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	FallbackBox->SetCollisionObjectType(ECC_WorldStatic);
	FallbackBox->SetCollisionResponseToAllChannels(ECR_Ignore);
	FallbackBox->SetCollisionResponseToChannel(EMBMeleeHitChannel, ECR_Overlap);
	FallbackBox->SetGenerateOverlapEvents(false);
	FallbackBox->SetCanEverAffectNavigation(false);
	FallbackBox->SetupAttachment(GetRootComponent());
	FallbackBox->RegisterComponent();

	if (HasAuthority())
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Récolte : %s n'a aucun mesh avec collision, boîte de repli ajoutée (corriger le Blueprint)."), *GetClass()->GetName());
	}
}

void AEMBResourceNode::ApplyAvailabilityState()
{
	// Jamais SetActorHiddenInGame / SetActorEnableCollision : un acteur masqué ou sans collision deviendrait non pertinent
	// pour le réseau et ne recevrait plus la réplication de sa réapparition. On agit sur chaque composant primitif.
	CacheComponentStates();
	const bool bAvailable = IsAvailable();
	for (const FComponentState& State : ComponentStates)
	{
		if (UPrimitiveComponent* Primitive = State.Component.Get())
		{
			Primitive->SetCollisionEnabled(bAvailable ? State.Collision.GetValue() : ECollisionEnabled::NoCollision);
			Primitive->SetHiddenInGame(bAvailable ? State.bHiddenInGame : true);
		}
	}
	BP_OnAvailabilityChanged(bAvailable);
}

void AEMBResourceNode::HandleEnvironmentChanged()
{
	RefreshNightState();
}

void AEMBResourceNode::RefreshNightState()
{
	// Sans sous-système d'environnement, le filon reste visible (mieux vaut une ressource disponible qu'un jeu bloqué).
	const UWorld* World = GetWorld();
	const UEMBEnvironmentSubsystem* Environment = World ? World->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr;
	const bool bShouldHide = Environment ? !Environment->IsNight() : false;
	if (bShouldHide != bHiddenByNight)
	{
		bHiddenByNight = bShouldHide;
		PushNetState();
	}
}

void AEMBResourceNode::Multicast_PlayHitFeedback_Implementation(const FVector_NetQuantize HitLocation)
{
	BP_OnHarvestHit(HitLocation);
}

void AEMBResourceNode::Multicast_PlayFellingCrack_Implementation(const float DurationSeconds)
{
	BP_OnFellingCrack(DurationSeconds);
}

void AEMBResourceNode::Multicast_SpawnFracture_Implementation(const TSoftClassPtr<AActor> InFractureClass)
{
	// Débris purement visuels : pas sur un serveur dédié (aucun écran).
	if (IsNetMode(NM_DedicatedServer) || InFractureClass.IsNull())
	{
		return;
	}
	PendingFractureClass = InFractureClass;
	FractureLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(InFractureClass.ToSoftObjectPath(),
		FStreamableDelegate::CreateUObject(this, &AEMBResourceNode::OnFractureClassLoaded));
}

void AEMBResourceNode::OnFractureClassLoaded()
{
	UWorld* World = GetWorld();
	UClass* DebrisClass = PendingFractureClass.Get();
	if (!World || !DebrisClass)
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (AActor* Debris = World->SpawnActor<AActor>(DebrisClass, GetActorTransform(), SpawnParameters))
	{
		Debris->SetLifeSpan(EMBHarvestDefaults::FractureDebrisSeconds);
	}
	FractureLoadHandle.Reset();
}
