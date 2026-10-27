// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBProgressionComponent.h"
#include "Emberfall.h"
#include "EMBBuildingComponent.h"
#include "EMBCombatComponent.h"
#include "EMBGameplayTags.h"
#include "EMBInventoryComponent.h"
#include "EMBProgressionSettings.h"
#include "EMBProgressionSubsystem.h"
#include "EMBSurvivalComponent.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Net/UnrealNetwork.h"

#define LOCTEXT_NAMESPACE "EMBProgression"

namespace
{
	/** Garde-fou contre un dépassement de int64 si une source d'XP aberrante était branchée. */
	constexpr int64 MaxTotalXP = MAX_int64;

	bool IsBossKilledTag(const FGameplayTag& Tag)
	{
		return Tag.MatchesTagExact(EMBTags::Event_Boss_Killed_Guardian.GetTag())
			|| Tag.MatchesTagExact(EMBTags::Event_Boss_Killed_Seraph.GetTag())
			|| Tag.MatchesTagExact(EMBTags::Event_Boss_Killed_Dragon.GetTag());
	}
}

UEMBProgressionComponent::UEMBProgressionComponent()
{
	PrimaryComponentTick.bCanEverTick = false; // Purement événementiel.
	SetIsReplicatedByDefault(true);

	Level = 1;
	TotalXP = 0;
	UnspentGlyphPoints = 0;
	UnspentAwakeningPoints = 0;
}

void UEMBProgressionComponent::BeginPlay()
{
	Super::BeginPlay();

	// Calcul initial des états dérivés (les tables sont chargées à l'initialisation du sous-système).
	RebuildDerivedState();
	RefreshGlyphStates();
	RefreshSkillRanks();
}

bool UEMBProgressionComponent::HasAuthorityOwner() const
{
	const AActor* Owner = GetOwner();
	return Owner && Owner->HasAuthority();
}

UEMBProgressionSubsystem* UEMBProgressionComponent::GetSubsystem() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetSubsystem<UEMBProgressionSubsystem>() : nullptr;
}

void UEMBProgressionComponent::MarkNetDirty() const
{
	// Le PlayerState se met à jour lentement par défaut : on force l'envoi, un achat doit se voir tout de suite.
	if (AActor* Owner = GetOwner())
	{
		Owner->ForceNetUpdate();
	}
}

void UEMBProgressionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// À tous : niveau et XP (affichage de groupe).
	DOREPLIFETIME(UEMBProgressionComponent, Level);
	DOREPLIFETIME(UEMBProgressionComponent, TotalXP);

	// Propriétaire seul : l'état détaillé n'intéresse personne d'autre.
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, UnspentGlyphPoints, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, UnspentAwakeningPoints, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, UnlockedGlyphs, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, RevealedGlyphs, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, SkillRanks, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, Knowledge, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UEMBProgressionComponent, ObtainedTriggers, COND_OwnerOnly);
}

// ================================================================================================
// XP et niveaux
// ================================================================================================

void UEMBProgressionComponent::AddXP(int64 Amount)
{
	if (!HasAuthorityOwner() || Amount <= 0)
	{
		return;
	}

	const UEMBProgressionSettings* Settings = UEMBProgressionSettings::Get();
	// Addition saturée : jamais de débordement de int64 (comportement indéfini), même avec une source aberrante.
	TotalXP = (TotalXP > MaxTotalXP - Amount) ? MaxTotalXP : TotalXP + Amount;

	// Boucle multi-niveaux : une grosse prime d'XP peut en faire passer plusieurs. Au plafond, l'XP excédentaire
	// reste dans TotalXP (elle n'est pas perdue, et ne donne plus de niveau).
	const int32 OldLevel = Level;
	while (Level < Settings->MaxLevel && TotalXP >= Settings->GetCumulativeXPForLevel(Level + 1))
	{
		++Level;
		UnspentGlyphPoints += Settings->GetGlyphPointsForLevel(Level);
		UnspentAwakeningPoints += Settings->AwakeningPointsPerLevel;
		OnLevelUp.Broadcast(Level);
	}

	OnXPChanged.Broadcast(TotalXP, Level);
	if (Level != OldLevel)
	{
		OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
		RefreshGlyphStates(); // Les niveaux requis ont pu être atteints.
	}
	MarkNetDirty();
}

void UEMBProgressionComponent::OnRep_Level(int32 OldLevel)
{
	// Première réception (connexion) : OldLevel est la valeur par défaut, pas un vrai niveau précédent.
	if (bLevelReceived && Level > OldLevel)
	{
		OnLevelUp.Broadcast(Level);
	}
	bLevelReceived = true;
	OnXPChanged.Broadcast(TotalXP, Level);
	RefreshGlyphStates();
}

void UEMBProgressionComponent::OnRep_TotalXP()
{
	OnXPChanged.Broadcast(TotalXP, Level);
}

void UEMBProgressionComponent::OnRep_Points()
{
	OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
}

// ================================================================================================
// Déclencheurs et savoirs
// ================================================================================================

void UEMBProgressionComponent::GrantTrigger(FGameplayTag EventTag)
{
	if (!HasAuthorityOwner() || !EventTag.IsValid() || ObtainedTriggers.HasTagExact(EventTag))
	{
		return; // Un déclencheur ne s'obtient qu'une fois.
	}

	ObtainedTriggers.AddTag(EventTag);
	const UEMBProgressionSettings* Settings = UEMBProgressionSettings::Get();

	// +1 point d'Éveil par boss majeur, une seule fois (garanti par le test ci-dessus).
	if (IsBossKilledTag(EventTag) && Settings->AwakeningPointsPerBoss > 0)
	{
		UnspentAwakeningPoints += Settings->AwakeningPointsPerBoss;
		OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
	}

	// Glyphes secrets liés à ce déclencheur : révélés (à acheter) ou débloqués gratuitement selon le mode.
	if (const UEMBProgressionSubsystem* Subsystem = GetSubsystem())
	{
		if (const UDataTable* Table = Subsystem->GetGlyphTable())
		{
			TArray<FName> ToProcess;
			Table->ForeachRow<FEMBGlyphRow>(TEXT("EMB GrantTrigger"), [&](const FName& Key, const FEMBGlyphRow& Row)
			{
				if (Row.bHidden && Row.SecretTrigger.IsValid() && Row.SecretTrigger.MatchesTagExact(EventTag))
				{
					ToProcess.Add(Key);
				}
			});

			for (const FName& GlyphId : ToProcess)
			{
				if (UnlockedGlyphs.Contains(GlyphId))
				{
					continue;
				}
				// AutoUnlock : gratuit, mais seulement si tous les AUTRES prérequis (niveau, parents, savoirs)
				// sont remplis ; sinon le glyphe est simplement révélé et restera à acheter plus tard.
				bool bAutoUnlock = false;
				if (Settings->SecretGlyphMode == EEMBSecretGlyphMode::AutoUnlock)
				{
					FText Ignored;
					const FEMBGlyphRow* Row = Subsystem->FindGlyph(GlyphId);
					bAutoUnlock = Row && CheckGlyphPrerequisites(*Row, Ignored);
				}

				if (bAutoUnlock)
				{
					UnlockGlyphInternal(GlyphId, /*bFree=*/true);
				}
				else
				{
					RevealedGlyphs.AddUnique(GlyphId);
				}
			}
		}
	}

	RefreshAll();
	MarkNetDirty();
}

void UEMBProgressionComponent::GrantKnowledge(FName KnowledgeId)
{
	if (!HasAuthorityOwner() || KnowledgeId.IsNone() || Knowledge.Contains(KnowledgeId))
	{
		return;
	}
	Knowledge.Add(KnowledgeId);
	RefreshGlyphStates(); // Un savoir peut rendre des glyphes disponibles.
	MarkNetDirty();
}

void UEMBProgressionComponent::OnRep_Knowledge()
{
	RefreshGlyphStates();
}

void UEMBProgressionComponent::OnRep_Triggers()
{
	RefreshGlyphStates();
}

// ================================================================================================
// Glyphes
// ================================================================================================

bool UEMBProgressionComponent::CheckGlyphPrerequisites(const FEMBGlyphRow& Row, FText& OutReason) const
{
	if (Level < Row.RequiredLevel)
	{
		OutReason = FText::Format(LOCTEXT("NeedLevel", "Niveau {0} requis."), FText::AsNumber(Row.RequiredLevel));
		return false;
	}

	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	for (const FName& Parent : Row.ParentGlyphs)
	{
		if (!Parent.IsNone() && !UnlockedGlyphs.Contains(Parent))
		{
			const FEMBGlyphRow* ParentRow = Subsystem ? Subsystem->FindGlyph(Parent) : nullptr;
			OutReason = FText::Format(LOCTEXT("NeedParent", "Glyphe requis : {0}."),
				ParentRow ? ParentRow->DisplayName : FText::FromName(Parent));
			return false;
		}
	}

	for (const FName& Required : Row.RequiredKnowledge)
	{
		if (!Required.IsNone() && !Knowledge.Contains(Required))
		{
			OutReason = FText::Format(LOCTEXT("NeedKnowledge", "Savoir requis : {0}."), FText::FromName(Required));
			return false;
		}
	}

	if (Row.SecretTrigger.IsValid() && !ObtainedTriggers.HasTagExact(Row.SecretTrigger))
	{
		OutReason = FText::Format(LOCTEXT("NeedTrigger", "Événement requis : {0}."), FText::FromName(Row.SecretTrigger.GetTagName()));
		return false;
	}
	return true;
}

EEMBGlyphState UEMBProgressionComponent::ComputeGlyphState(FName GlyphId, const FEMBGlyphRow& Row) const
{
	if (UnlockedGlyphs.Contains(GlyphId))
	{
		return EEMBGlyphState::Unlocked;
	}

	// Glyphe secret : invisible tant que son déclencheur n'est pas obtenu (ou qu'il n'a pas été révélé).
	const bool bTriggerObtained = !Row.SecretTrigger.IsValid() || ObtainedTriggers.HasTagExact(Row.SecretTrigger);
	if (Row.bHidden && !RevealedGlyphs.Contains(GlyphId) && !bTriggerObtained)
	{
		return EEMBGlyphState::Hidden;
	}

	FText Ignored;
	return CheckGlyphPrerequisites(Row, Ignored) ? EEMBGlyphState::Available : EEMBGlyphState::Locked;
}

EEMBGlyphState UEMBProgressionComponent::GetGlyphState(FName GlyphId) const
{
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	const FEMBGlyphRow* Row = Subsystem ? Subsystem->FindGlyph(GlyphId) : nullptr;
	return Row ? ComputeGlyphState(GlyphId, *Row) : EEMBGlyphState::Hidden;
}

bool UEMBProgressionComponent::HasGlyph(FName GlyphId) const
{
	return UnlockedGlyphs.Contains(GlyphId);
}

bool UEMBProgressionComponent::HasGrantedTag(FGameplayTag Tag) const
{
	return Tag.IsValid() && GrantedTags.HasTagExact(Tag);
}

bool UEMBProgressionComponent::CanUnlockGlyph(FName GlyphId, FText& OutReason) const
{
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	const FEMBGlyphRow* Row = Subsystem ? Subsystem->FindGlyph(GlyphId) : nullptr;
	if (!Row)
	{
		OutReason = LOCTEXT("UnknownGlyph", "Glyphe inconnu.");
		return false;
	}

	switch (ComputeGlyphState(GlyphId, *Row))
	{
	case EEMBGlyphState::Unlocked:
		OutReason = LOCTEXT("AlreadyKnown", "Glyphe déjà appris.");
		return false;
	case EEMBGlyphState::Hidden:
		OutReason = LOCTEXT("HiddenGlyph", "Glyphe secret non révélé.");
		return false;
	default:
		break;
	}

	if (!CheckGlyphPrerequisites(*Row, OutReason))
	{
		return false;
	}
	if (UnspentGlyphPoints < Row->Cost)
	{
		OutReason = FText::Format(LOCTEXT("NeedGlyphPoints", "{0} points de glyphe requis."), FText::AsNumber(Row->Cost));
		return false;
	}
	return true;
}

void UEMBProgressionComponent::UnlockGlyphInternal(FName GlyphId, bool bFree)
{
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	const FEMBGlyphRow* Row = Subsystem ? Subsystem->FindGlyph(GlyphId) : nullptr;
	if (!Row || UnlockedGlyphs.Contains(GlyphId))
	{
		return;
	}

	if (!bFree)
	{
		UnspentGlyphPoints = FMath::Max(0, UnspentGlyphPoints - Row->Cost);
		OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
	}
	UnlockedGlyphs.AddUnique(GlyphId);
}

void UEMBProgressionComponent::RequestUnlockGlyph(FName GlyphId)
{
	Server_UnlockGlyph(GlyphId);
}

bool UEMBProgressionComponent::Server_UnlockGlyph_Validate(FName GlyphId)
{
	// Toujours vrai : un identifiant périmé ou inconnu (table modifiée, interface en retard) n'est pas une
	// triche, et un Validate faux déconnecte le joueur. Le serveur refuse proprement via CanUnlockGlyph.
	return true;
}

bool UEMBProgressionComponent::AllowRequest()
{
	// Plafond de requêtes par joueur et par seconde (fenêtre d'une seconde) : protège le serveur d'un client
	// qui inonde de RPC ; les requêtes en trop sont ignorées, jamais punies par une déconnexion.
	constexpr int32 MaxRequestsPerSecond = 10;
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - RequestWindowStart >= 1.0)
	{
		RequestWindowStart = Now;
		RequestCount = 0;
	}
	return ++RequestCount <= MaxRequestsPerSecond;
}

void UEMBProgressionComponent::Client_ActionRejected_Implementation(FName Id, const FText& Reason)
{
	OnActionRejected.Broadcast(Id, Reason);
}

void UEMBProgressionComponent::Server_UnlockGlyph_Implementation(FName GlyphId)
{
	if (!AllowRequest())
	{
		return; // Trop de requêtes : ignorée sans déconnexion (un double-clic humain reste très en dessous).
	}

	// Le serveur revérifie TOUT : le client n'est jamais cru sur parole.
	FText Reason;
	if (!CanUnlockGlyph(GlyphId, Reason))
	{
		UE_LOG(LogEmberfall, Verbose, TEXT("Achat de glyphe %s refusé : %s"), *GlyphId.ToString(), *Reason.ToString());
		Client_ActionRejected(GlyphId, Reason); // Le joueur voit pourquoi (interface).
		return;
	}

	UnlockGlyphInternal(GlyphId, /*bFree=*/false);
	RefreshAll();
	MarkNetDirty();
}

void UEMBProgressionComponent::OnRep_Glyphs()
{
	RebuildDerivedState();
	RefreshGlyphStates();
}

// ================================================================================================
// Compétences (Skill Tree)
// ================================================================================================

int32 UEMBProgressionComponent::GetSkillRank(FName NodeId) const
{
	for (const FEMBSkillRank& Entry : SkillRanks)
	{
		if (Entry.NodeId == NodeId)
		{
			return Entry.Rank;
		}
	}
	return 0;
}

bool UEMBProgressionComponent::CanRankUpSkill(FName NodeId, FText& OutReason) const
{
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	const FEMBSkillNodeRow* Row = Subsystem ? Subsystem->FindSkill(NodeId) : nullptr;
	if (!Row)
	{
		OutReason = LOCTEXT("UnknownSkill", "Compétence inconnue.");
		return false;
	}
	if (GetSkillRank(NodeId) >= Row->MaxRank)
	{
		OutReason = LOCTEXT("MaxRank", "Rang maximal atteint.");
		return false;
	}
	if (Level < Row->RequiredLevel)
	{
		OutReason = FText::Format(LOCTEXT("SkillNeedLevel", "Niveau {0} requis."), FText::AsNumber(Row->RequiredLevel));
		return false;
	}
	for (const FName& Parent : Row->ParentNodes)
	{
		if (!Parent.IsNone() && GetSkillRank(Parent) < 1)
		{
			const FEMBSkillNodeRow* ParentRow = Subsystem->FindSkill(Parent);
			OutReason = FText::Format(LOCTEXT("SkillNeedParent", "Compétence requise : {0}."),
				ParentRow ? ParentRow->DisplayName : FText::FromName(Parent));
			return false;
		}
	}
	for (const FName& Required : Row->RequiredKnowledge)
	{
		if (!Required.IsNone() && !Knowledge.Contains(Required))
		{
			OutReason = FText::Format(LOCTEXT("SkillNeedKnowledge", "Savoir requis : {0}."), FText::FromName(Required));
			return false;
		}
	}
	if (UnspentAwakeningPoints < Row->Cost)
	{
		OutReason = FText::Format(LOCTEXT("NeedAwakening", "{0} points d'Éveil requis."), FText::AsNumber(Row->Cost));
		return false;
	}
	return true;
}

void UEMBProgressionComponent::RequestRankUpSkill(FName NodeId)
{
	Server_RankUpSkill(NodeId);
}

bool UEMBProgressionComponent::Server_RankUpSkill_Validate(FName NodeId)
{
	return true; // Voir Server_UnlockGlyph_Validate : le serveur refuse via CanRankUpSkill.
}

void UEMBProgressionComponent::Server_RankUpSkill_Implementation(FName NodeId)
{
	if (!AllowRequest())
	{
		return;
	}

	FText Reason;
	if (!CanRankUpSkill(NodeId, Reason))
	{
		UE_LOG(LogEmberfall, Verbose, TEXT("Rang de compétence %s refusé : %s"), *NodeId.ToString(), *Reason.ToString());
		Client_ActionRejected(NodeId, Reason);
		return;
	}

	const FEMBSkillNodeRow* Row = GetSubsystem()->FindSkill(NodeId);
	UnspentAwakeningPoints = FMath::Max(0, UnspentAwakeningPoints - Row->Cost);

	bool bFound = false;
	for (FEMBSkillRank& Entry : SkillRanks)
	{
		if (Entry.NodeId == NodeId)
		{
			++Entry.Rank;
			bFound = true;
			break;
		}
	}
	if (!bFound)
	{
		FEMBSkillRank NewRank;
		NewRank.NodeId = NodeId;
		NewRank.Rank = 1;
		SkillRanks.Add(NewRank);
	}

	OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
	RefreshAll();
	MarkNetDirty();
}

void UEMBProgressionComponent::OnRep_SkillRanks()
{
	RebuildDerivedState();
	RefreshSkillRanks();
}

// ================================================================================================
// État dérivé : tags accordés, passifs, délégués
// ================================================================================================

void UEMBProgressionComponent::RebuildDerivedState()
{
	GrantedTags.Reset();
	Modifiers = FEMBPassiveModifiers();

	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	if (!Subsystem)
	{
		return;
	}

	for (const FName& GlyphId : UnlockedGlyphs)
	{
		if (const FEMBGlyphRow* Row = Subsystem->FindGlyph(GlyphId))
		{
			if (Row->GrantedTag.IsValid())
			{
				GrantedTags.AddTag(Row->GrantedTag);
			}
		}
	}

	// Passifs : somme des ValuePerRank x rang, par statistique.
	for (const FEMBSkillRank& Entry : SkillRanks)
	{
		if (Entry.Rank <= 0)
		{
			continue;
		}
		if (const FEMBSkillNodeRow* Row = Subsystem->FindSkill(Entry.NodeId))
		{
			if (Row->GrantedTag.IsValid())
			{
				GrantedTags.AddTag(Row->GrantedTag);
			}
			Modifiers.Add(Row->PassiveStat, Row->ValuePerRank * static_cast<float>(Entry.Rank));
		}
	}
}

void UEMBProgressionComponent::RefreshGlyphStates()
{
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();
	const UDataTable* Table = Subsystem ? Subsystem->GetGlyphTable() : nullptr;
	if (!Table)
	{
		return;
	}

	// Parcours de toute la table : appelé seulement lors d'un changement (niveau, achat, savoir, déclencheur).
	TArray<TPair<FName, EEMBGlyphState>> Changes;
	Table->ForeachRow<FEMBGlyphRow>(TEXT("EMB RefreshGlyphStates"), [&](const FName& Key, const FEMBGlyphRow& Row)
	{
		const EEMBGlyphState NewState = ComputeGlyphState(Key, Row);
		const EEMBGlyphState* Old = CachedGlyphStates.Find(Key);
		if (!Old || *Old != NewState)
		{
			// Premier calcul (bGlyphsSeeded faux) : on mémorise sans diffuser, l'UI lit l'état initial à l'ouverture.
			if (bGlyphsSeeded)
			{
				Changes.Emplace(Key, NewState);
			}
			CachedGlyphStates.Add(Key, NewState);
		}
	});
	bGlyphsSeeded = true;

	for (const TPair<FName, EEMBGlyphState>& Change : Changes)
	{
		OnGlyphStateChanged.Broadcast(Change.Key, Change.Value);
	}
}

void UEMBProgressionComponent::RefreshSkillRanks()
{
	// Rangs actuels (inclut les nœuds repassés à 0).
	TMap<FName, int32> Current;
	for (const FEMBSkillRank& Entry : SkillRanks)
	{
		Current.Add(Entry.NodeId, Entry.Rank);
	}

	TArray<TPair<FName, int32>> Changes;
	for (const TPair<FName, int32>& Pair : Current)
	{
		const int32* Old = CachedSkillRanks.Find(Pair.Key);
		if ((Old ? *Old : 0) != Pair.Value)
		{
			Changes.Emplace(Pair.Key, Pair.Value);
		}
	}
	for (const TPair<FName, int32>& Pair : CachedSkillRanks)
	{
		if (Pair.Value != 0 && !Current.Contains(Pair.Key))
		{
			Changes.Emplace(Pair.Key, 0);
		}
	}
	CachedSkillRanks = MoveTemp(Current);

	// Même règle que pour les glyphes : le premier calcul est mémorisé sans diffusion.
	if (bSkillsSeeded)
	{
		for (const TPair<FName, int32>& Change : Changes)
		{
			OnSkillRankChanged.Broadcast(Change.Key, Change.Value);
		}
	}
	bSkillsSeeded = true;
}

void UEMBProgressionComponent::RefreshAll()
{
	RebuildDerivedState();
	RefreshGlyphStates();
	RefreshSkillRanks();
	if (HasAuthorityOwner())
	{
		PushModifiersToCurrentPawn();
	}
}

// ================================================================================================
// Application au pawn
// ================================================================================================

void UEMBProgressionComponent::PushModifiersToCurrentPawn()
{
	// Le composant pousse lui-même vers le pawn courant : le personnage n'a pas à s'abonner.
	if (const APlayerState* PlayerState = Cast<APlayerState>(GetOwner()))
	{
		ApplyModifiersToPawn(PlayerState->GetPawn());
	}
}

void UEMBProgressionComponent::ApplyModifiersToPawn(APawn* Pawn)
{
	if (!HasAuthorityOwner() || !Pawn)
	{
		return;
	}

	if (UEMBCombatComponent* Combat = Pawn->FindComponentByClass<UEMBCombatComponent>())
	{
		Combat->SetPassiveModifiers(Modifiers);
	}
	if (UEMBBuildingComponent* Building = Pawn->FindComponentByClass<UEMBBuildingComponent>())
	{
		// BuildCost est un bonus additif négatif (-0,10 par rang) : 2 rangs => x0,8. Jamais négatif.
		Building->SetBuildCostMultiplier(FMath::Max(0.f, 1.f + Modifiers.BuildCost));
	}
	if (UEMBInventoryComponent* Inventory = Pawn->FindComponentByClass<UEMBInventoryComponent>())
	{
		Inventory->SetCarryWeightBonus(Modifiers.CarryWeight);
	}
	if (UEMBSurvivalComponent* Survival = Pawn->FindComponentByClass<UEMBSurvivalComponent>())
	{
		// Survie : HungerThirstRate, ComfortRange, CorruptionGain, FireResistance (et HarvestYield exposé).
		Survival->SetPassiveModifiers(Modifiers);
	}
	// La résistance au feu s'applique aussi directement : le combat lit FireResistance dans ses propres
	// modificateurs (SetPassiveModifiers ci-dessus), quel que soit le composant de survie.
}

// ================================================================================================
// Sauvegarde
// ================================================================================================

void UEMBProgressionComponent::ExportSaveData(FEMBProgressionSaveData& Out) const
{
	Out.Level = Level;
	Out.TotalXP = TotalXP;
	Out.UnspentGlyphPoints = UnspentGlyphPoints;
	Out.UnspentAwakeningPoints = UnspentAwakeningPoints;
	Out.UnlockedGlyphs = UnlockedGlyphs;
	Out.RevealedGlyphs = RevealedGlyphs;
	Out.SkillRanks = SkillRanks;
	Out.Knowledge = Knowledge;
	Out.ObtainedTriggers = ObtainedTriggers;
}

void UEMBProgressionComponent::ImportSaveData(const FEMBProgressionSaveData& In)
{
	if (!HasAuthorityOwner())
	{
		return;
	}

	const UEMBProgressionSettings* Settings = UEMBProgressionSettings::Get();
	const UEMBProgressionSubsystem* Subsystem = GetSubsystem();

	// Les tables ne servent à filtrer que si elles sont chargées : une table absente ne doit pas effacer
	// toute la progression d'un joueur.
	const bool bCanFilterGlyphs = Subsystem && Subsystem->GetGlyphTable();
	const bool bCanFilterSkills = Subsystem && Subsystem->GetSkillTable();

	/*
	 * Assainissement : une sauvegarde corrompue ou éditée à la main ne doit pas produire un état impossible.
	 * Le niveau est DÉDUIT de l'XP (source de vérité), pas lu tel quel.
	 */
	TotalXP = FMath::Clamp<int64>(In.TotalXP, 0, MaxTotalXP);
	Level = 1;
	while (Level < Settings->MaxLevel && TotalXP >= Settings->GetCumulativeXPForLevel(Level + 1))
	{
		++Level;
	}

	// Points : jamais au-delà de ce que le niveau et les boss vaincus permettent d'avoir gagné.
	int32 MaxGlyphPoints = 0;
	for (int32 L = 2; L <= Level; ++L)
	{
		MaxGlyphPoints += Settings->GetGlyphPointsForLevel(L);
	}
	int32 BossesKilled = 0;
	for (const FGameplayTag& Tag : In.ObtainedTriggers)
	{
		BossesKilled += IsBossKilledTag(Tag) ? 1 : 0;
	}
	const int32 MaxAwakeningPoints = (Level - 1) * Settings->AwakeningPointsPerLevel + BossesKilled * Settings->AwakeningPointsPerBoss;
	UnspentGlyphPoints = FMath::Clamp(In.UnspentGlyphPoints, 0, MaxGlyphPoints);
	UnspentAwakeningPoints = FMath::Clamp(In.UnspentAwakeningPoints, 0, MaxAwakeningPoints);

	// Glyphes : doublons retirés, identifiants inconnus ignorés (glyphe supprimé d'une version à l'autre).
	UnlockedGlyphs.Reset();
	for (const FName& Id : In.UnlockedGlyphs)
	{
		if (!Id.IsNone() && (!bCanFilterGlyphs || Subsystem->FindGlyph(Id)))
		{
			UnlockedGlyphs.AddUnique(Id);
		}
	}
	RevealedGlyphs.Reset();
	for (const FName& Id : In.RevealedGlyphs)
	{
		if (!Id.IsNone() && (!bCanFilterGlyphs || Subsystem->FindGlyph(Id)))
		{
			RevealedGlyphs.AddUnique(Id);
		}
	}

	// Rangs : nœuds inconnus ignorés, doublons fusionnés (le rang le plus haut gagne), plafonnés à MaxRank.
	SkillRanks.Reset();
	for (const FEMBSkillRank& Entry : In.SkillRanks)
	{
		if (Entry.NodeId.IsNone() || Entry.Rank <= 0)
		{
			continue;
		}
		const FEMBSkillNodeRow* Row = Subsystem ? Subsystem->FindSkill(Entry.NodeId) : nullptr;
		if (bCanFilterSkills && !Row)
		{
			continue;
		}
		const int32 Rank = Row ? FMath::Min(Entry.Rank, Row->MaxRank) : Entry.Rank;

		FEMBSkillRank* Existing = SkillRanks.FindByPredicate([&Entry](const FEMBSkillRank& Other) { return Other.NodeId == Entry.NodeId; });
		if (Existing)
		{
			Existing->Rank = FMath::Max(Existing->Rank, Rank);
		}
		else
		{
			FEMBSkillRank Clean;
			Clean.NodeId = Entry.NodeId;
			Clean.Rank = Rank;
			SkillRanks.Add(Clean);
		}
	}

	// Savoirs dédoublonnés ; déclencheurs invalides retirés (le conteneur est déjà sans doublon).
	Knowledge.Reset();
	for (const FName& Id : In.Knowledge)
	{
		if (!Id.IsNone())
		{
			Knowledge.AddUnique(Id);
		}
	}
	ObtainedTriggers.Reset();
	for (const FGameplayTag& Tag : In.ObtainedTriggers)
	{
		if (Tag.IsValid())
		{
			ObtainedTriggers.AddTag(Tag);
		}
	}

	// Caches remis à zéro : recalculés sans diffusion de chaque glyphe / rang ; l'UI relit l'état complet
	// grâce à OnXPChanged / OnPointsChanged diffusés ci-dessous (et à GetGlyphState à l'ouverture de l'arbre).
	CachedGlyphStates.Reset();
	CachedSkillRanks.Reset();
	bGlyphsSeeded = false;
	bSkillsSeeded = false;
	RefreshAll();

	OnXPChanged.Broadcast(TotalXP, Level);
	OnPointsChanged.Broadcast(UnspentGlyphPoints, UnspentAwakeningPoints);
	MarkNetDirty();
}

#undef LOCTEXT_NAMESPACE
