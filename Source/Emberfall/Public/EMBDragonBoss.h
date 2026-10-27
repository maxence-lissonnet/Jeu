// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "EMBBossBase.h"
#include "EMBDragonBoss.generated.h"

/**
 * Vaelthyrax, l'Ancêtre-Braise : boss final (dragon ancestral, 3 phases).
 *
 * Toute la mécanique générique (stats, phases, recharges, dégâts de zone, vol scripté, défaite, sauvegarde...) vit
 * dans AEMBBossBase. Cette classe ne porte que ce qui est propre au dragon :
 *  - la configuration chiffrée de la section 6 du référentiel (constantes EMBDragonDefaults) ;
 *  - le Cataclysm scripté (entrée en phase 3 + 20 s, puis toutes les 75 s) ;
 *  - le poids x3 de la rafale d'ailes quand au moins deux joueurs sont collés au dragon.
 * Les tâches BT propres au dragon (souffle, piqué) s'appuient sur l'API publique de la base.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBDragonBoss : public AEMBBossBase
{
	GENERATED_BODY()

public:
	AEMBDragonBoss();

	// --- AEMBBossBase ---
	virtual FGameplayTag GetDefeatedEventTag() const override;
	virtual float GetAttackWeightModifier(const FEMBBossAttackRow& Row) const override;
	virtual bool IsScriptedAttackDue() const override { return IsCataclysmDue(); }

	/** Cataclysm scripté : entrée en phase 3 + 20 s, puis toutes les 75 s. */
	bool IsCataclysmDue() const;
	void NotifyCataclysmFired();

protected:
	virtual void OnPhaseEnteredServer(int32 NewPhase, int32 OldPhase) override;

	/** Rencontre reprise après un abandon (wipe) alors que le dragon est déjà en phase 3 : Cataclysm 20 s après la reprise. */
	virtual void OnEncounterBegun() override;

private:
	double NextCataclysmTime = -1.0;
};
