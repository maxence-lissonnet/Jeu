// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBProgressionSubsystem.h"
#include "Emberfall.h"
#include "EMBPlayerState.h"
#include "EMBProgressionComponent.h"
#include "EMBProgressionSettings.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"

bool UEMBProgressionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Mondes de jeu uniquement (pas les mondes d'aperçu de l'éditeur) ; serveur ET clients.
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->IsGameWorld();
}

void UEMBProgressionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LoadTables();
}

void UEMBProgressionSubsystem::LoadTables()
{
	const UEMBProgressionSettings* Settings = UEMBProgressionSettings::Get();

	GlyphTable = Settings->GlyphTable.LoadSynchronous();
	SkillTable = Settings->SkillTable.LoadSynchronous();

	// Contrôle du type de ligne : FindRow<T> sur une table d'une autre structure lirait de la mémoire invalide.
	if (GlyphTable && GlyphTable->GetRowStruct() != FEMBGlyphRow::StaticStruct())
	{
		UE_LOG(LogEmberfall, Error, TEXT("Progression : %s n'utilise pas la structure FEMBGlyphRow, table ignorée."), *GetNameSafe(GlyphTable));
		GlyphTable = nullptr;
	}
	if (SkillTable && SkillTable->GetRowStruct() != FEMBSkillNodeRow::StaticStruct())
	{
		UE_LOG(LogEmberfall, Error, TEXT("Progression : %s n'utilise pas la structure FEMBSkillNodeRow, table ignorée."), *GetNameSafe(SkillTable));
		SkillTable = nullptr;
	}

	if (!GlyphTable)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Progression : DataTable des glyphes introuvable ou invalide (%s)."), *Settings->GlyphTable.ToString());
	}
	if (!SkillTable)
	{
		UE_LOG(LogEmberfall, Warning, TEXT("Progression : DataTable des compétences introuvable ou invalide (%s)."), *Settings->SkillTable.ToString());
	}
}

const FEMBGlyphRow* UEMBProgressionSubsystem::FindGlyph(FName GlyphId) const
{
	return (GlyphTable && !GlyphId.IsNone()) ? GlyphTable->FindRow<FEMBGlyphRow>(GlyphId, TEXT("EMB FindGlyph"), false) : nullptr;
}

const FEMBSkillNodeRow* UEMBProgressionSubsystem::FindSkill(FName NodeId) const
{
	return (SkillTable && !NodeId.IsNone()) ? SkillTable->FindRow<FEMBSkillNodeRow>(NodeId, TEXT("EMB FindSkill"), false) : nullptr;
}

UEMBProgressionComponent* UEMBProgressionSubsystem::GetProgression(const APlayerState* Player)
{
	const AEMBPlayerState* EMBState = Cast<AEMBPlayerState>(Player);
	return EMBState ? EMBState->GetProgressionComponent() : nullptr;
}

bool UEMBProgressionSubsystem::IsWorldEventRecorded(FGameplayTag EventTag) const
{
	return EventTag.IsValid() && WorldEvents.HasTagExact(EventTag);
}

void UEMBProgressionSubsystem::RestoreWorldEvents(const FGameplayTagContainer& Events)
{
	WorldEvents = Events;
}

void UEMBProgressionSubsystem::NotifyWorldEvent(FGameplayTag EventTag, AActor* Source, int64 XPReward, float ParticipantRadius)
{
	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client || !EventTag.IsValid())
	{
		return; // Serveur uniquement.
	}

	// Événement déjà enregistré (boss déjà vaincu, y compris restauré d'une sauvegarde) : rien à redistribuer.
	if (WorldEvents.HasTagExact(EventTag))
	{
		return;
	}

	const AGameStateBase* GameState = World->GetGameState();
	if (GameState)
	{
		const float RadiusSquared = FMath::Square(ParticipantRadius);
		for (APlayerState* Player : GameState->PlayerArray)
		{
			UEMBProgressionComponent* Progression = GetProgression(Player);
			if (!Progression)
			{
				continue;
			}

			// Participant : pawn (vivant ou mort, tant qu'il existe) à portée de la source de l'événement.
			if (Source)
			{
				const APawn* Pawn = Player->GetPawn();
				if (!Pawn || FVector::DistSquared(Pawn->GetActorLocation(), Source->GetActorLocation()) > RadiusSquared)
				{
					continue;
				}
			}

			// L'XP n'est versée qu'à la première obtention du déclencheur par ce joueur (pas de répétition).
			const bool bFirstTime = !Progression->HasObtainedTrigger(EventTag);
			Progression->GrantTrigger(EventTag);
			if (bFirstTime && XPReward > 0)
			{
				Progression->AddXP(XPReward);
			}
		}
	}

	// Enregistré APRÈS la distribution : l'autosave déclenchée par le délégué voit la progression à jour
	// (XP, points et déclencheurs de chaque participant déjà versés).
	WorldEvents.AddTag(EventTag);
	OnWorldEventRecorded.Broadcast(EventTag);
}

void UEMBProgressionSubsystem::NotifyPlayerEvent(APlayerState* Player, FGameplayTag EventTag, int64 XPReward)
{
	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client || !EventTag.IsValid())
	{
		return;
	}

	UEMBProgressionComponent* Progression = GetProgression(Player);
	if (!Progression)
	{
		return;
	}

	const bool bFirstTime = !Progression->HasObtainedTrigger(EventTag);
	Progression->GrantTrigger(EventTag);
	if (bFirstTime && XPReward > 0)
	{
		Progression->AddXP(XPReward);
	}
}
