// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBDiscoveryVolume.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBProgressionSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

AEMBDiscoveryVolume::AEMBDiscoveryVolume()
{
	PrimaryActorTick.bCanEverTick = false;
	XPReward = 0;

	Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Box"));
	SetRootComponent(Box);
	Box->SetBoxExtent(FVector(300.f, 300.f, 200.f));
	Box->SetCollisionProfileName(TEXT("OverlapOnlyPawn"));
	Box->SetGenerateOverlapEvents(true);
}

void AEMBDiscoveryVolume::BeginPlay()
{
	Super::BeginPlay();

	// Serveur uniquement : le client n'a ni à détecter ni à récompenser.
	if (HasAuthority())
	{
		Box->OnComponentBeginOverlap.AddDynamic(this, &AEMBDiscoveryVolume::OnVolumeBeginOverlap);
	}
}

void AEMBDiscoveryVolume::OnVolumeBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	const APawn* Pawn = Cast<APawn>(OtherActor);
	AEMBPlayerState* PlayerState = Pawn ? Pawn->GetPlayerState<AEMBPlayerState>() : nullptr;
	UEMBProgressionComponent* Progression = PlayerState ? PlayerState->GetProgressionComponent() : nullptr;
	if (!Progression)
	{
		return; // Pas un joueur (PNJ, créature...).
	}

	// Le savoir d'abord : la première découverte peut débloquer des glyphes qui exigent ce savoir.
	const bool bNewKnowledge = !GrantedKnowledge.IsNone() && !Progression->HasKnowledge(GrantedKnowledge);
	if (!GrantedKnowledge.IsNone())
	{
		Progression->GrantKnowledge(GrantedKnowledge);
	}

	if (DiscoveryTag.IsValid())
	{
		// Le sous-système verse l'XP uniquement à la première obtention du déclencheur par ce joueur.
		if (UEMBProgressionSubsystem* Subsystem = GetWorld()->GetSubsystem<UEMBProgressionSubsystem>())
		{
			Subsystem->NotifyPlayerEvent(PlayerState, DiscoveryTag, XPReward);
		}
	}
	else if (bNewKnowledge && XPReward > 0)
	{
		// Sans étiquette, la récompense n'est versée qu'une fois, à la première obtention du savoir.
		Progression->AddXP(XPReward);
	}
}
