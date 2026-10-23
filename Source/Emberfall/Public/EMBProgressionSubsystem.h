// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMBProgressionTypes.h"
#include "EMBProgressionSubsystem.generated.h"

class APlayerState;
class UDataTable;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnWorldEventRecorded, FGameplayTag, EventTag);

/**
 * Règles et routage de la progression, sans état de joueur (celui-ci vit dans UEMBProgressionComponent
 * sur le PlayerState) :
 *  - possède les DataTables des glyphes et des nœuds de compétence (créé aussi sur les clients : l'UI et
 *    les composants y lisent les définitions) ;
 *  - enregistre les événements monde Event.* (boss vaincus) et les distribue aux joueurs concernés.
 * La distribution (XP, déclencheurs) ne s'exécute que sur le serveur.
 */
UCLASS()
class EMBERFALL_API UEMBProgressionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/**
	 * Événement monde (boss vaincu...) : les participants sont les joueurs dont le pawn est à moins de
	 * ParticipantRadius de Source (Source nul = tous les joueurs). Chacun reçoit le déclencheur et l'XP,
	 * puis l'événement est enregistré. Serveur uniquement.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void NotifyWorldEvent(FGameplayTag EventTag, AActor* Source, int64 XPReward, float ParticipantRadius);

	/** Événement propre à un joueur (lieu découvert). Serveur uniquement. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Progression")
	void NotifyPlayerEvent(APlayerState* Player, FGameplayTag EventTag, int64 XPReward);

	const FEMBGlyphRow* FindGlyph(FName GlyphId) const;
	const FEMBSkillNodeRow* FindSkill(FName NodeId) const;

	/** Tables chargées (nullptr si le chemin des réglages est invalide) ; pour parcourir tous les glyphes / nœuds. */
	const UDataTable* GetGlyphTable() const { return GlyphTable; }
	const UDataTable* GetSkillTable() const { return SkillTable; }

	/**
	 * SERVEUR UNIQUEMENT : les événements monde ne sont ni répliqués ni enregistrés sur les clients (leur
	 * liste y est vide). Un client déduit un boss vaincu de l'état répliqué de l'acteur concerné.
	 */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Progression")
	bool IsWorldEventRecorded(FGameplayTag EventTag) const;

	/** Chargement d'une sauvegarde : remplace les événements monde sans diffuser le délégué. */
	void RestoreWorldEvents(const FGameplayTagContainer& Events);

	/** Pour la sauvegarde. */
	const FGameplayTagContainer& GetWorldEvents() const { return WorldEvents; }

	/** Écouté par la sauvegarde (autosave sur boss vaincu). */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Progression")
	FEMBOnWorldEventRecorded OnWorldEventRecorded;

private:
	/** Cherche le composant de progression d'un PlayerState (nullptr si ce n'est pas un AEMBPlayerState). */
	static class UEMBProgressionComponent* GetProgression(const APlayerState* Player);

	/**
	 * Chargement SYNCHRONE au démarrage du monde : les deux tables sont petites (quelques centaines de
	 * lignes), chargées une fois, et un chargement asynchrone ouvrirait une fenêtre où les achats et les
	 * états de glyphes seraient indéterminés. À passer en asynchrone si les tables grossissent beaucoup.
	 */
	void LoadTables();

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> GlyphTable;

	UPROPERTY(Transient)
	TObjectPtr<UDataTable> SkillTable;

	FGameplayTagContainer WorldEvents;
};
