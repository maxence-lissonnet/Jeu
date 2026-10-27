// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "TimerManager.h"
#include "EMBCreatureSignificance.generated.h"

class AEMBCreatureBase;

/** Palier de détail d'une créature, selon sa distance au joueur le plus proche (référentiel §16). */
UENUM(BlueprintType)
enum class EEMBCreatureDetailTier : uint8
{
	/** < 3 000 cm : IA et animation pleines. */
	Full,
	/** 3 000 à 8 000 cm : tick IA toutes les 0,25 s, mise à jour d'animation réduite (URO). */
	Reduced,
	/** > 8 000 cm : IA en pause (BT arrêté), mesh sans tick. */
	Paused
};

/**
 * Registre des créatures vivantes et gestion de leur niveau de détail (plugin SignificanceManager).
 *
 * Existe sur le serveur ET sur chaque client (UWorldSubsystem de monde de jeu) :
 *  - serveur : compteur global de créatures vivantes (budget de 40, consulté par les spawners), pause de l'IA,
 *    tick IA espacé, retour des créatures trop lointaines à leur spawner (> 15 000 cm du joueur le plus proche) ;
 *  - serveur et clients : réglage de la mise à jour d'animation du mesh (URO, tick du mesh) selon la distance au
 *    joueur LOCAL le plus proche (un client ne connaît que ses propres joueurs) ; aucun contrôleur IA chez les clients.
 *
 * Fonctionnement : chaque créature est enregistrée dans USignificanceManager avec une fonction de significance
 * (SignificanceMaxDistance - distance au point de vue) ; toutes les 0,5 s le sous-système appelle Update() avec la
 * position des joueurs (le gestionnaire retient le meilleur point de vue, donc le joueur le plus proche), relit la
 * significance de chaque créature et en déduit la distance, puis le palier (avec hystérésis de 250 cm).
 *
 * PRÉREQUIS : le gestionnaire USignificanceManager n'existe que si le plugin est actif ET si sa classe est configurée
 * dans les réglages du projet (voir le compte rendu). Sans gestionnaire, un journal d'avertissement est émis une fois
 * et la distance est calculée directement : le comportement reste identique, seule l'intégration est contournée.
 */
UCLASS()
class EMBERFALL_API UEMBCreatureSignificance : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	/** Enregistre une créature vivante (budget + significance). Idempotent. Serveur et clients. */
	void RegisterCreature(AEMBCreatureBase* Creature);

	/** Retire une créature (mort, destruction). Idempotent. */
	void UnregisterCreature(AEMBCreatureBase* Creature);

	/** Créatures vivantes enregistrées : le compteur global du budget serveur. */
	int32 GetLiveCreatureCount() const;

	/** Palier pour une distance donnée, avec hystérésis par rapport au palier courant. */
	static EEMBCreatureDetailTier ComputeTier(float Distance, EEMBCreatureDetailTier Current);

private:
	void UpdateSignificance();
	void ApplyTier(AEMBCreatureBase& Creature, EEMBCreatureDetailTier Tier) const;

	/** Créatures suivies -> palier courant. Références faibles : un acteur détruit est purgé à la mise à jour. */
	TMap<TWeakObjectPtr<AEMBCreatureBase>, EEMBCreatureDetailTier> Creatures;

	FTimerHandle UpdateTimerHandle;
	bool bLoggedMissingManager = false;

	/** Heure monde depuis laquelle aucun point de vue n'existe (-1 : au moins un joueur). */
	double NoViewpointSince = -1.0;
};
