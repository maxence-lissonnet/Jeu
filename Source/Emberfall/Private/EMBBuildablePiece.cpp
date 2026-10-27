// Emberfall - pièce de construction répliquée (Dev B).

#include "EMBBuildablePiece.h"

#include "Emberfall.h"
#include "EMBEnvironmentSubsystem.h"
#include "EMBRespawnSubsystem.h"
#include "EMBStructureSubsystem.h"
#include "EMBSurvivalComponent.h"
#include "EMBSurvivalTypes.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerState.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/World.h"
#include "Engine/DamageEvents.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "PhysicsEngine/BodySetup.h"

namespace
{
	/** Préfixe des sockets d'accrochage. */
	const TCHAR* const SnapSocketPrefix = TEXT("Snap_");

	/** Index des données primitives personnalisées lues par le matériau de la pièce. */
	constexpr int32 CPDIndexIntegrity = 0; // 0..1 (intégrité / 100)
	constexpr int32 CPDIndexHealth = 1;    // 0..1 (PV / PV max)
}

AEMBBuildablePiece::AEMBBuildablePiece()
{
	// Une pièce ne bouge jamais et n'a rien à faire à chaque frame : aucun Tick (des milliers d'instances).
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false);

	// DORM_Initial : dormante dès le départ pour les pièces posées dans le niveau.
	// Les pièces créées en jeu passent en DORM_DormantAll dans BeginPlay (après leur réplication initiale).
	NetDormancy = DORM_Initial;

	// Une pièce change très rarement d'état : fréquence de mise à jour basse (elle est de toute façon dormante).
	SetNetUpdateFrequency(2.f);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	SetRootComponent(Mesh);
	Mesh->SetMobility(EComponentMobility::Movable); // Movable requis pour pouvoir simuler la physique à l'effondrement.
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	// EMBMeleeHit (ECC_GameTraceChannel1) : Ignore, pour que les traces de mêlée traversent les pièces sans bloquer
	// (les coups sont résolus par le combat, pas par la collision du décor construit).
	Mesh->SetCollisionResponseToChannel(ECC_GameTraceChannel1, ECR_Ignore);
	// EMBBuilding (ECC_GameTraceChannel2) : Block, canal des tests de placement et de chevauchement.
	Mesh->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block);
	// Aucun événement d'overlap sur des milliers de pièces : inutile et coûteux.
	Mesh->SetGenerateOverlapEvents(false);
}

void AEMBBuildablePiece::PostInitializeComponents()
{
	Super::PostInitializeComponents();

	// Appliqué ici (et non dans le constructeur) pour prendre en compte la valeur définie dans le Blueprint.
	SetNetCullDistanceSquared(FMath::Square(NetCullDistance));

	// Réglage explicite de la participation au NavMesh (voir bAffectNavigation dans l'en-tête).
	if (Mesh)
	{
		Mesh->SetCanEverAffectNavigation(bAffectNavigation);
	}
}

void AEMBBuildablePiece::InitializePiece(FName InRowName, EEMBBuildMaterial InMaterial, EEMBPieceCategory InCategory, bool bInAnchoredToGround, int32 InPlacerPlayerId)
{
	if (!HasAuthority())
	{
		return;
	}

	bPlacedByPlayer = true;
	PlacerPlayerId = InPlacerPlayerId;
	PieceRowName = InRowName;
	Material = InMaterial;
	Category = InCategory;
	bAnchoredToGround = bInAnchoredToGround;
	CurrentHealth = GetMaxHealth();
}

void AEMBBuildablePiece::ApplyRowFunctions(const FEMBBuildPieceRow& Row)
{
	if (!HasAuthority())
	{
		return;
	}
	bIsRespawnPoint = Row.bIsRespawnPoint;
	// Bornes : une table mal renseignée ne doit pas produire un rayon négatif ou infini.
	// Mêmes bornes que l'environnement (EMBSurvivalDefaults) : une valeur acceptée ici ne sera pas rognée plus loin.
	HeatRadius = FMath::Clamp(Row.HeatRadius, 0.f, 5000.f);
	HeatBonus = FMath::Clamp(Row.HeatBonus, -EMBSurvivalDefaults::MaxHeatBonusC, EMBSurvivalDefaults::MaxHeatBonusC);
	PurifyRadius = FMath::Clamp(Row.PurifyRadius, 0.f, 5000.f);
	PurifyRate = FMath::Clamp(Row.PurifyRate, 0.f, EMBSurvivalDefaults::MaxPurifierRate);
}

void AEMBBuildablePiece::RegisterEnvironmentFunctions()
{
	if (!HasAuthority() || bEnvironmentRegistered || bCollapsed || (HeatRadius <= 0.f && PurifyRadius <= 0.f))
	{
		return;
	}
	if (UEMBEnvironmentSubsystem* Environment = GetWorld() ? GetWorld()->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr)
	{
		if (HeatRadius > 0.f)
		{
			Environment->RegisterHeatSource(this, HeatRadius, HeatBonus);
		}
		if (PurifyRadius > 0.f)
		{
			Environment->RegisterPurifier(this, PurifyRadius, PurifyRate);
		}
		bEnvironmentRegistered = true;
	}
}

void AEMBBuildablePiece::UnregisterEnvironmentFunctions()
{
	if (!bEnvironmentRegistered)
	{
		return;
	}
	bEnvironmentRegistered = false;
	if (UEMBEnvironmentSubsystem* Environment = GetWorld() ? GetWorld()->GetSubsystem<UEMBEnvironmentSubsystem>() : nullptr)
	{
		Environment->UnregisterHeatSource(this);
		Environment->UnregisterPurifier(this);
	}
}

bool AEMBBuildablePiece::IsSheltered() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Trace vers le haut depuis le dessus de la pièce. EMBBuilding (ECC_GameTraceChannel2) est bloqué par les
	// pièces de construction : toucher une pièce = un toit, un plafond ou un étage au-dessus = à l'abri.
	const FBox Bounds = GetStructureBounds();
	const FVector Start(Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.Max.Z + 5.0);
	const FVector End = Start + FVector(0.0, 0.0, EMBBuildingDefaults::ShelterTraceHeight);

	FCollisionQueryParams Params(SCENE_QUERY_STAT(EMBShelterTrace), /*bTraceComplex*/ false, this);
	FHitResult Hit;
	return World->LineTraceSingleByChannel(Hit, Start, End, ECC_GameTraceChannel2, Params) && Cast<AEMBBuildablePiece>(Hit.GetActor()) != nullptr;
}

bool AEMBBuildablePiece::CanInteract(const APawn* InInstigator) const
{
	// Serveur et client (invite d'interface) : n'importe quel joueur vivant avec un PlayerState peut se lier.
	return bIsRespawnPoint && !bCollapsed && InInstigator && InInstigator->GetPlayerState();
}

FText AEMBBuildablePiece::GetInteractionText(const APawn* InInstigator) const
{
	if (IsBed())
	{
		return NSLOCTEXT("Emberfall", "BindAndSleep", "Se lier au lit et dormir");
	}
	return NSLOCTEXT("Emberfall", "BindRespawnPoint", "Se lier à ce point de réapparition");
}

void AEMBBuildablePiece::Interact(APawn* InInstigator)
{
	if (!HasAuthority() || !CanInteract(InInstigator))
	{
		return;
	}

	UEMBRespawnSubsystem* Respawn = UEMBRespawnSubsystem::Get(this);

	// Le dernier lien remplace le précédent (géré par le sous-système). La liaison est instantanée et illimitée.
	if (Respawn)
	{
		Respawn->BindRespawnPoint(InInstigator->GetPlayerState(), this);
	}

	// Une seule interaction : se lier ET dormir. Dormir réduit la Corruption, mais seulement à l'abri et au plus une
	// fois toutes les 120 s par joueur (sinon un lit deviendrait un purificateur illimité).
	if (IsBed() && IsSheltered())
	{
		if (UEMBSurvivalComponent* Survival = InInstigator->FindComponentByClass<UEMBSurvivalComponent>())
		{
			if (Respawn && Respawn->TryConsumeSleepBenefit(InInstigator->GetController()))
			{
				Survival->ReduceCorruption(EMBBuildingDefaults::BedCorruptionReduction);
			}
		}
	}
}

void AEMBBuildablePiece::RestoreFromSave(float InHealth, const FString& InPlacerStableId)
{
	if (!HasAuthority())
	{
		return;
	}
	// Au moins 1 PV : une pièce sauvegardée vivante ne doit pas s'effondrer à cause d'une valeur corrompue.
	CurrentHealth = FMath::Clamp(InHealth, 1.f, GetMaxHealth());
	PlacerStableId = InPlacerStableId;
}

float AEMBBuildablePiece::GetMaxHealth() const
{
	return EMBBuildingDefaults::GetMaterialStats(Material).MaxHealth * HealthMultiplier;
}

void AEMBBuildablePiece::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority() && CurrentHealth <= 0.f && !bCollapsed)
	{
		// Pièce posée à la main dans le niveau : InitializePiece n'a pas été appelé.
		CurrentHealth = GetMaxHealth();
	}

	UpdateVisualState();

	// Enregistrement dans le graphe, côté serveur ET côté client : le client en a besoin pour la prédiction
	// d'intégrité de l'aperçu (adjacence + intégrité répliquée). Seul le serveur calcule et fait s'effondrer.
	if (UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this))
	{
		Structure->RegisterPiece(this);
	}

	if (HasAuthority())
	{
		// Chaleur / purification : enregistrées auprès du sous-système d'environnement (serveur seulement).
		RegisterEnvironmentFunctions();

		// Dormance totale : le moteur effectue d'abord la réplication initiale vers les clients concernés, puis
		// l'acteur s'endort. Indispensable pour tenir des milliers de pièces (voir l'en-tête de classe).
		SetNetDormancy(DORM_DormantAll);
	}
}

void AEMBBuildablePiece::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterEnvironmentFunctions();

	if (UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this))
	{
		// Recalcul des voisins seulement si la pièce disparaît réellement en cours de partie
		// (pas pendant un changement de niveau ou la fermeture du jeu).
		Structure->UnregisterPiece(this, EndPlayReason == EEndPlayReason::Destroyed);
	}

	Super::EndPlay(EndPlayReason);
}

void AEMBBuildablePiece::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Identité : envoyée une seule fois dans le bunch initial (bande passante nulle ensuite).
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, PieceRowName, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, Material, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, Category, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, bAnchoredToGround, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, PlacerPlayerId, COND_InitialOnly);
	DOREPLIFETIME_CONDITION(AEMBBuildablePiece, bIsRespawnPoint, COND_InitialOnly);

	// État dynamique : ne part que lorsque le serveur réveille la pièce (FlushNetDormancy).
	DOREPLIFETIME(AEMBBuildablePiece, CurrentHealth);
	DOREPLIFETIME(AEMBBuildablePiece, Integrity);
	DOREPLIFETIME(AEMBBuildablePiece, bCollapsed);
}

FBox AEMBBuildablePiece::GetBoundsForTransform(const FTransform& InTransform) const
{
	const FBox LocalBox(PlacementBoxCenterOffset - PlacementExtent, PlacementBoxCenterOffset + PlacementExtent);
	return LocalBox.TransformBy(InTransform);
}

void AEMBBuildablePiece::GetSnapSocketsLocal(FEMBSnapSocketArray& OutSockets) const
{
	if (!bLocalSocketsCached)
	{
		CachedLocalSockets.Reset();

		if (Mesh)
		{
			if (const UStaticMesh* StaticMesh = Mesh->GetStaticMesh())
			{
				// Le mesh est la racine : ses sockets, exprimés dans l'espace du composant, SONT déjà dans l'espace
				// de l'acteur. Utiliser GetRelativeTransform() d'une racine renverrait son transform MONDE (pas de
				// parent) et le doublerait lors du passage en espace monde. On ne compose donc qu'un éventuel
				// décalage réel (mesh non racine dans un Blueprint dérivé).
				const FTransform MeshToActor = (Mesh == GetRootComponent()) ? FTransform::Identity : Mesh->GetRelativeTransform();

				for (const UStaticMeshSocket* Socket : StaticMesh->Sockets)
				{
					if (Socket && Socket->SocketName.ToString().StartsWith(SnapSocketPrefix))
					{
						FEMBSnapSocket& Entry = CachedLocalSockets.AddDefaulted_GetRef();
						Entry.Name = Socket->SocketName;
						Entry.Transform = FTransform(Socket->RelativeRotation, Socket->RelativeLocation, Socket->RelativeScale) * MeshToActor;
					}
				}
			}
		}

		for (const FEMBSnapSocket& Extra : ExtraSnapSockets)
		{
			CachedLocalSockets.Add(Extra);
		}

		// <Kind> précalculé une fois : "Snap_Edge_N" -> "Edge". Deux sockets sont compatibles si leur Kind est identique.
		for (FEMBSnapSocket& Entry : CachedLocalSockets)
		{
			TArray<FString> Parts;
			Entry.Name.ToString().ParseIntoArray(Parts, TEXT("_"), /*bCullEmpty*/ true);
			Entry.Kind = Parts.Num() >= 2 ? FName(*Parts[1]) : Entry.Name;
		}
		bLocalSocketsCached = true;
	}

	OutSockets = CachedLocalSockets;
}

void AEMBBuildablePiece::GetSnapSocketsWorld(FEMBSnapSocketArray& OutSockets) const
{
	GetSnapSocketsLocal(OutSockets);

	const FTransform ActorTransform = GetActorTransform();
	for (FEMBSnapSocket& Socket : OutSockets)
	{
		Socket.Transform = Socket.Transform * ActorTransform;
	}
}

void AEMBBuildablePiece::GetSupportNeighbors(TArray<AEMBBuildablePiece*>& OutNeighbors) const
{
	OutNeighbors.Reset();
	if (const UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this))
	{
		Structure->GetNeighbors(this, OutNeighbors);
	}
}

float AEMBBuildablePiece::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	// Pont avec le système de dégâts standard du moteur : tout ApplyDamage sur une pièce devient un dégât structurel.
	return ApplyStructuralDamage(DamageAmount, DamageCauser);
}

float AEMBBuildablePiece::ApplyStructuralDamage(float Amount, AActor* DamageCauser)
{
	// PV déjà à 0 : la destruction est en attente (regroupée en fin de frame), les coups suivants sont sans effet.
	if (!HasAuthority() || bCollapsed || Amount <= 0.f || CurrentHealth <= 0.f)
	{
		return 0.f;
	}

	const float Applied = FMath::Min(Amount, CurrentHealth);
	CurrentHealth -= Applied;

	// Propriété répliquée modifiée sur un acteur dormant : sans ce flush, aucun client ne verrait le changement.
	WakeForReplication();
	UpdateVisualState();

	if (CurrentHealth <= 0.f)
	{
		// Passe par le subsystem : retrait du graphe, recalcul des voisins, cascade éventuelle.
		Collapse();
	}

	return Applied;
}

EEMBHitResult AEMBBuildablePiece::ReceiveEMBHit(const FEMBHitData& Hit)
{
	if (!HasAuthority() || bCollapsed)
	{
		return EEMBHitResult::Ignored;
	}

	// Coop PvE : les joueurs ne détruisent pas leurs constructions à l'arme (sauf option PvP).
	if (!bAllowPlayerStructureDamage)
	{
		const APawn* InstigatorPawn = Cast<APawn>(Hit.Instigator);
		if (InstigatorPawn && InstigatorPawn->IsPlayerControlled())
		{
			return EEMBHitResult::Ignored;
		}
	}

	// Multiplicateur par type de dégâts : le feu détruit le bois (x1,5) mais glisse sur le reste (x0,5).
	float Multiplier = 0.5f;
	switch (Hit.DamageType)
	{
	case EEMBDamageType::Physical:
		Multiplier = 1.0f;
		break;
	case EEMBDamageType::Fire:
		Multiplier = (Material == EEMBBuildMaterial::Wood) ? 1.5f : 0.5f;
		break;
	default:
		Multiplier = 0.5f;
		break;
	}

	ApplyStructuralDamage(Hit.Damage * Multiplier, Hit.Instigator);
	return CurrentHealth <= 0.f ? EEMBHitResult::Killed : EEMBHitResult::Hit;
}

void AEMBBuildablePiece::Collapse()
{
	if (!HasAuthority() || bCollapsed)
	{
		return;
	}

	if (UEMBStructureSubsystem* Structure = UEMBStructureSubsystem::Get(this))
	{
		// Les destructions directes d'une même frame (souffle de dragon : des dizaines de pièces) sont regroupées
		// par le subsystem et traitées en UN seul retrait + UN seul recalcul de graphe.
		Structure->RequestCollapse(this);
	}
	else
	{
		ExecuteCollapse(true);
	}
}

void AEMBBuildablePiece::WakeForReplication()
{
	// FlushNetDormancy force la réplication sans changer l'état de dormance (la pièce se rendormira) ;
	// ForceNetUpdate évite d'attendre le prochain cycle selon NetUpdateFrequency (2 Hz).
	FlushNetDormancy();
	ForceNetUpdate();
}

void AEMBBuildablePiece::SetIntegrityServer(float NewIntegrity)
{
	if (!HasAuthority())
	{
		return;
	}

	NewIntegrity = FMath::Clamp(NewIntegrity, 0.f, EMBBuildingDefaults::GroundedIntegrity);
	if (FMath::IsNearlyEqual(NewIntegrity, Integrity, 0.01f))
	{
		// Optimisation : pas de réveil de dormance pour une valeur inchangée (recalcul d'une grande composante).
		return;
	}

	Integrity = NewIntegrity;
	WakeForReplication();
	UpdateVisualState();
}

void AEMBBuildablePiece::ExecuteCollapse(bool bHeavyDebris)
{
	if (!HasAuthority() || bCollapsed)
	{
		return;
	}

	bCollapsed = true;
	Integrity = 0.f;
	CurrentHealth = 0.f;

	// Un feu effondré ne chauffe plus et un brasero effondré ne purifie plus, même pendant le délai avant destruction.
	UnregisterEnvironmentFunctions();

	// Côté serveur la collision disparaît tout de suite : les requêtes de placement et les déplacements
	// ne doivent plus tenir compte de la pièce, même pendant le délai avant destruction.
	ApplyCollapsedCollision();
	UpdateVisualState();

	WakeForReplication();
	Multicast_PlayCollapseEffects(bHeavyDebris);

	// Destruction différée : laisse aux clients le temps de rejouer l'effet. La destruction elle-même est
	// répliquée automatiquement (fermeture du canal de l'acteur).
	SetLifeSpan(CollapseLifetime);
}

void AEMBBuildablePiece::Multicast_PlayCollapseEffects_Implementation(bool bHeavyDebris)
{
	// Serveur dédié : rien à afficher, on économise tout le coût de simulation et de rendu.
	if (IsNetMode(NM_DedicatedServer))
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || !Mesh)
	{
		return;
	}

	// Poussière : peu coûteux, joué pour toutes les pièces.
	// Chargement synchrone d'un petit asset ; à précharger via l'Asset Manager si un hitch apparaît au profilage.
	if (UNiagaraSystem* Dust = CollapseDustEffect.LoadSynchronous())
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(World, Dust, Mesh->Bounds.Origin);
	}

	if (bHeavyDebris)
	{
		// 1) GeometryCollection Chaos (le plus coûteux : fracture + dizaines de corps rigides).
		if (UClass* DebrisClass = CollapseDebrisClass.LoadSynchronous())
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AActor* Debris = World->SpawnActor<AActor>(DebrisClass, GetActorTransform(), SpawnParams))
			{
				Debris->SetLifeSpan(DebrisLifetime);
				Mesh->SetVisibility(false);
				return;
			}
		}

		// 2) Simulation physique du mesh d'origine (un seul corps rigide : bien moins coûteux).
		const UStaticMesh* StaticMesh = Mesh->GetStaticMesh();
		const UBodySetup* BodySetup = StaticMesh ? StaticMesh->GetBodySetup() : nullptr;
		if (bSimulatePhysicsOnCollapse && BodySetup && BodySetup->AggGeom.GetElementCount() > 0)
		{
			// Les débris ne collisionnent qu'avec le décor statique (pas avec les joueurs ni les autres débris).
			Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
			Mesh->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
			Mesh->SetSimulatePhysics(true);
			Mesh->AddImpulse(FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.5f) * 150.f, NAME_None, /*bVelChange*/ true);
			return;
		}
	}

	// 3) Version légère (pièces au-delà du quota de la vague) : la pièce disparaît avec sa poussière.
	Mesh->SetVisibility(false);
}

void AEMBBuildablePiece::ApplyCollapsedCollision()
{
	if (Mesh && !Mesh->IsSimulatingPhysics())
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void AEMBBuildablePiece::UpdateVisualState()
{
	// Serveur dédié : aucun rendu, donc rien à mettre à jour.
	if (!Mesh || IsNetMode(NM_DedicatedServer))
	{
		return;
	}

	// OPTIMISATION : Custom Primitive Data plutôt qu'un MaterialInstanceDynamic par pièce.
	// Un MID par instance casse le regroupement des draw calls et l'instanciation de ces milliers de meshes
	// identiques ; le CPD voyage avec l'instance GPU et garde tout le batching. Le matériau de la pièce lit
	// les index 0 (intégrité) et 1 (PV) via le noeud "CustomPrimitiveData" pour afficher fissures / teinte.
	const float MaxHealth = GetMaxHealth();
	Mesh->SetCustomPrimitiveDataFloat(CPDIndexIntegrity, Integrity / EMBBuildingDefaults::GroundedIntegrity);
	Mesh->SetCustomPrimitiveDataFloat(CPDIndexHealth, MaxHealth > 0.f ? CurrentHealth / MaxHealth : 0.f);
}

void AEMBBuildablePiece::OnRep_Integrity()
{
	UpdateVisualState();
}

void AEMBBuildablePiece::OnRep_CurrentHealth()
{
	UpdateVisualState();
}

void AEMBBuildablePiece::OnRep_Collapsed()
{
	// Filet de sécurité pour un client qui rejoint pendant le délai de destruction (le multicast est non fiable).
	if (bCollapsed)
	{
		ApplyCollapsedCollision();
	}
}
