// Emberfall - sac de mort (Dev B, référentiel section 9).

#include "EMBDeathBag.h"

#include "Emberfall.h"
#include "EMBRespawnSubsystem.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"

AEMBDeathBag::AEMBDeathBag()
{
	PrimaryActorTick.bCanEverTick = false;

	bReplicates = true;
	SetReplicateMovement(false);
	NetDormancy = DORM_Initial; // sans effet pour un acteur créé en jeu : voir le commentaire de classe
	SetNetUpdateFrequency(1.f);

	// Racine : sphère d'interaction. Le sac est ramassable même si aucun mesh n'est renseigné.
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	SetRootComponent(Collision);
	Collision->InitSphereRadius(60.f);
	Collision->SetMobility(EComponentMobility::Movable);
	Collision->SetGenerateOverlapEvents(false);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionResponseToAllChannels(ECR_Ignore);
	Collision->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block); // traces d'interaction
	Collision->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);     // détection par recouvrement
	Collision->SetCanEverAffectNavigation(false);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCanEverAffectNavigation(false);
}

void AEMBDeathBag::InitializeBag(const FString& InOwnerKey, int32 InOwnerPlayerId, bool bInPersistent, const TArray<FEMBResourceStack>& InContents)
{
	if (!HasAuthority())
	{
		return;
	}
	OwnerKey = InOwnerKey;
	OwnerPlayerId = InOwnerPlayerId;
	bPersistent = bInPersistent;
	Contents = InContents;
	SetNetCullDistanceSquared(FMath::Square(NetCullDistance));
}

void AEMBDeathBag::SetOwnerPlayerId(int32 InOwnerPlayerId)
{
	if (!HasAuthority() || OwnerPlayerId == InOwnerPlayerId)
	{
		return;
	}
	OwnerPlayerId = InOwnerPlayerId;
	// Au cas où l'acteur serait dormant : réveil + envoi immédiat de la propriété modifiée.
	FlushNetDormancy();
	ForceNetUpdate();
}

void AEMBDeathBag::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority())
	{
		if (UEMBRespawnSubsystem* Respawn = UEMBRespawnSubsystem::Get(this))
		{
			Respawn->NotifyBagDestroyed(this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AEMBDeathBag::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AEMBDeathBag, OwnerPlayerId);
}

bool AEMBDeathBag::CanInteract(const APawn* InInstigator) const
{
	const APlayerState* PlayerState = InInstigator ? InInstigator->GetPlayerState() : nullptr;
	if (!PlayerState)
	{
		return false;
	}

	if (HasAuthority())
	{
		// Autorité : comparaison des clés stables (le PlayerId de session peut ne pas encore être attribué).
		const UEMBRespawnSubsystem* Respawn = UEMBRespawnSubsystem::Get(this);
		return Respawn && !OwnerKey.IsEmpty() && Respawn->GetPlayerKey(InInstigator->GetController()) == OwnerKey;
	}

	// Client : seul le PlayerId de session est connu.
	return OwnerPlayerId != INDEX_NONE && PlayerState->GetPlayerId() == OwnerPlayerId;
}

FText AEMBDeathBag::GetInteractionText(const APawn* InInstigator) const
{
	return NSLOCTEXT("Emberfall", "PickUpDeathBag", "Ramasser le sac");
}

void AEMBDeathBag::Interact(APawn* InInstigator)
{
	if (!HasAuthority() || !CanInteract(InInstigator))
	{
		return;
	}

	UEMBInventoryComponent* Inventory = InInstigator->FindComponentByClass<UEMBInventoryComponent>();
	if (!Inventory)
	{
		return;
	}

	// Transfert mesuré : AddResource plafonne à MaxStackPerResource, on compare donc le compte avant/après pour savoir
	// ce qui est réellement passé. Ce qui n'est pas passé reste dans le sac (rien n'est perdu, le joueur peut revenir).
	TArray<FEMBResourceStack> Remaining;
	for (const FEMBResourceStack& Stack : Contents)
	{
		const int32 Before = Inventory->GetResourceCount(Stack.ResourceId);
		Inventory->AddResource(Stack.ResourceId, Stack.Count);
		const int32 Transferred = Inventory->GetResourceCount(Stack.ResourceId) - Before;
		if (Transferred < Stack.Count)
		{
			FEMBResourceStack& Left = Remaining.AddDefaulted_GetRef();
			Left.ResourceId = Stack.ResourceId;
			Left.Count = Stack.Count - FMath::Max(Transferred, 0);
		}
	}

	Contents = MoveTemp(Remaining);
	if (Contents.IsEmpty())
	{
		Destroy();
	}
}
