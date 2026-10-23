// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "EMBProgressionTypes.h"
#include "EMBProgressionComponent.generated.h"

class APawn;
class UEMBProgressionSubsystem;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnXPChanged, int64, TotalXP, int32, Level);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnLevelUp, int32, NewLevel);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnPointsChanged, int32, GlyphPoints, int32, AwakeningPoints);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnGlyphStateChanged, FName, GlyphId, EEMBGlyphState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnSkillRankChanged, FName, NodeId, int32, NewRank);
/** Le serveur a refusé un achat de glyphe ou un rang de compétence : identifiant concerné et motif lisible. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnActionRejected, FName, Id, FText, Reason);

/**
 * Progression d'un joueur (niveau, XP, points, glyphes, rangs de compétences, savoirs, déclencheurs).
 * Vit sur AEMBPlayerState : il survit à la mort du pawn et se réplique.
 *
 * RÉSEAU
 *  - Toute modification se fait sur le SERVEUR. Le client appelle RequestUnlockGlyph / RequestRankUpSkill
 *    (RPC Server Reliable avec validation) et le serveur revérifie toutes les conditions.
 *  - Niveau et XP total sont répliqués à tous (UI de groupe). Le reste (points, glyphes, rangs, savoirs,
 *    déclencheurs) est réservé au propriétaire (COND_OwnerOnly) : les autres joueurs n'en ont pas besoin.
 *  - Ce sont des TArray (jamais de TMap, non répliquable). Les RepNotify recalculent l'état dérivé et
 *    rediffusent les délégués d'UI côté client.
 *  - Les bonus de passifs sont poussés vers le pawn courant à chaque changement (côté serveur).
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent))
class EMBERFALL_API UEMBProgressionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBProgressionComponent();

	// ---------------------------------------------------------------- Contrat (section 7.4)

	/** Serveur. Ajoute de l'XP ; gère plusieurs niveaux d'un coup, plafonné au niveau maximal (l'excédent reste dans TotalXP). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void AddXP(int64 Amount);

	/** Serveur. Acquiert un événement Event.* (une seule fois) : points d'Éveil des boss, révélation ou déblocage des glyphes secrets. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void GrantTrigger(FGameplayTag EventTag);

	/** Serveur. Ajoute un savoir (lore) au joueur. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void GrantKnowledge(FName KnowledgeId);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	EEMBGlyphState GetGlyphState(FName GlyphId) const;

	/** Vérifie tous les prérequis (niveau, parents, savoirs, déclencheur, points) ; OutReason explique le premier manquant. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	bool CanUnlockGlyph(FName GlyphId, FText& OutReason) const;

	/** Client -> serveur (Server_UnlockGlyph). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void RequestUnlockGlyph(FName GlyphId);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	bool CanRankUpSkill(FName NodeId, FText& OutReason) const;

	/** Client -> serveur (Server_RankUpSkill). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void RequestRankUpSkill(FName NodeId);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	bool HasGlyph(FName GlyphId) const;

	/** Tags accordés par les glyphes appris et les compétences possédées (GrantedTag). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	bool HasGrantedTag(FGameplayTag Tag) const;

	const FEMBPassiveModifiers& GetPassiveModifiers() const { return Modifiers; }

	/** Serveur. Pousse les bonus de passifs vers les composants du pawn (combat, construction, inventaire). */
	void ApplyModifiersToPawn(APawn* Pawn);

	void ExportSaveData(FEMBProgressionSaveData& Out) const;

	/** Serveur. Remplace la progression, recalcule et réapplique les modificateurs. */
	void ImportSaveData(const FEMBProgressionSaveData& In);

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnXPChanged OnXPChanged;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnLevelUp OnLevelUp;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnPointsChanged OnPointsChanged;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnGlyphStateChanged OnGlyphStateChanged;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnSkillRankChanged OnSkillRankChanged;
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression") FEMBOnActionRejected OnActionRejected;

	// ---------------------------------------------------------------- Lecture

	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") int32 GetLevel() const { return Level; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") int64 GetTotalXP() const { return TotalXP; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") int32 GetUnspentGlyphPoints() const { return UnspentGlyphPoints; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") int32 GetUnspentAwakeningPoints() const { return UnspentAwakeningPoints; }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") int32 GetSkillRank(FName NodeId) const;
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") bool HasKnowledge(FName KnowledgeId) const { return Knowledge.Contains(KnowledgeId); }
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression") bool HasObtainedTrigger(FGameplayTag EventTag) const { return EventTag.IsValid() && ObtainedTriggers.HasTagExact(EventTag); }

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// RPC (le PlayerState appartient à la connexion du PlayerController : RPC autorisées)
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_UnlockGlyph(FName GlyphId);

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_RankUpSkill(FName NodeId);

	/** Refus d'une action : explique au joueur pourquoi. Non fiable (information d'interface seulement). */
	UFUNCTION(Client, Unreliable)
	void Client_ActionRejected(FName Id, const FText& Reason);

	/** Serveur : vrai si la requête respecte le plafond (10 par seconde). */
	bool AllowRequest();

	double RequestWindowStart = 0.0;
	int32 RequestCount = 0;

	// RepNotify
	UFUNCTION() void OnRep_Level(int32 OldLevel);
	UFUNCTION() void OnRep_TotalXP();
	UFUNCTION() void OnRep_Points();
	UFUNCTION() void OnRep_Glyphs();
	UFUNCTION() void OnRep_SkillRanks();
	UFUNCTION() void OnRep_Knowledge();
	UFUNCTION() void OnRep_Triggers();

	bool HasAuthorityOwner() const;
	UEMBProgressionSubsystem* GetSubsystem() const;

	EEMBGlyphState ComputeGlyphState(FName GlyphId, const FEMBGlyphRow& Row) const;
	bool CheckGlyphPrerequisites(const FEMBGlyphRow& Row, FText& OutReason) const;
	void UnlockGlyphInternal(FName GlyphId, bool bFree);

	/** Recalcule tags accordés et bonus de passifs depuis les glyphes et rangs possédés. */
	void RebuildDerivedState();
	/** Compare états de glyphes / rangs au dernier état connu et rediffuse les délégués des changements. */
	void RefreshGlyphStates();
	void RefreshSkillRanks();
	/** Recalcul complet (dérivés + délégués) ; côté serveur, pousse aussi les bonus vers le pawn. */
	void RefreshAll();
	void PushModifiersToCurrentPawn();
	void MarkNetDirty() const;

	// ---- Répliqué à tous ----
	UPROPERTY(ReplicatedUsing = OnRep_Level) int32 Level;
	UPROPERTY(ReplicatedUsing = OnRep_TotalXP) int64 TotalXP;

	// ---- Répliqué au propriétaire seul ----
	UPROPERTY(ReplicatedUsing = OnRep_Points) int32 UnspentGlyphPoints;
	UPROPERTY(ReplicatedUsing = OnRep_Points) int32 UnspentAwakeningPoints;
	UPROPERTY(ReplicatedUsing = OnRep_Glyphs) TArray<FName> UnlockedGlyphs;
	UPROPERTY(ReplicatedUsing = OnRep_Glyphs) TArray<FName> RevealedGlyphs;
	UPROPERTY(ReplicatedUsing = OnRep_SkillRanks) TArray<FEMBSkillRank> SkillRanks;
	UPROPERTY(ReplicatedUsing = OnRep_Knowledge) TArray<FName> Knowledge;
	UPROPERTY(ReplicatedUsing = OnRep_Triggers) FGameplayTagContainer ObtainedTriggers;

	// ---- Dérivé (recalculé sur chaque machine, non répliqué) ----
	FGameplayTagContainer GrantedTags;
	FEMBPassiveModifiers Modifiers;
	TMap<FName, EEMBGlyphState> CachedGlyphStates;
	TMap<FName, int32> CachedSkillRanks;

	/**
	 * Premier calcul des glyphes / rangs : mémorisé SANS diffusion (l'UI lit l'état initial à l'ouverture),
	 * pour les deux de la même façon. Remis à faux par ImportSaveData, qui prévient l'UI par OnXPChanged /
	 * OnPointsChanged à la place d'une rafale de délégués par glyphe.
	 */
	bool bGlyphsSeeded = false;
	bool bSkillsSeeded = false;

	/** Faux tant que le niveau n'a pas été reçu une première fois : évite un faux OnLevelUp à la connexion. */
	bool bLevelReceived = false;
};
