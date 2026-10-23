// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/DeveloperSettings.h"
#include "EMBProgressionTypes.h"
#include "EMBProgressionSettings.generated.h"

/**
 * Réglages de progression et de sauvegarde, lus dans Config/DefaultGame.ini
 * (section [/Script/Emberfall.EMBProgressionSettings]) et éditables dans Project Settings > Game.
 * Valeurs par défaut = référentiel section 7.2.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Emberfall - Progression"))
class EMBERFALL_API UEMBProgressionSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UEMBProgressionSettings();

	/** Accès aux réglages (objet par défaut de la classe). */
	static const UEMBProgressionSettings* Get() { return GetDefault<UEMBProgressionSettings>(); }

	// ---- Données ----

	// RequiredAssetDataTags filtre le sélecteur d'assets sur la structure de ligne ; le type est aussi
	// revérifié au chargement (UEMBProgressionSubsystem::LoadTables).
	UPROPERTY(Config, EditAnywhere, Category = "Data", meta = (RequiredAssetDataTags = "RowStructure=/Script/Emberfall.EMBGlyphRow"))
	TSoftObjectPtr<UDataTable> GlyphTable;

	UPROPERTY(Config, EditAnywhere, Category = "Data", meta = (RequiredAssetDataTags = "RowStructure=/Script/Emberfall.EMBSkillNodeRow"))
	TSoftObjectPtr<UDataTable> SkillTable;

	// ---- Règles à valider ----

	/** RevealOnly : le déclencheur révèle le glyphe secret (à acheter). AutoUnlock : débloqué gratuitement. */
	UPROPERTY(Config, EditAnywhere, Category = "Rules")
	EEMBSecretGlyphMode SecretGlyphMode;

	/** Joueur mort au moment de la sauvegarde : dernier point sûr (PV pleins) ou position telle quelle. Lu par la sauvegarde. */
	UPROPERTY(Config, EditAnywhere, Category = "Rules")
	EEMBDeathSaveBehavior DeathSaveBehavior;

	// ---- XP ----

	/** XP pour passer du niveau L à L+1 = round(XPBase x L^XPExponent). */
	UPROPERTY(Config, EditAnywhere, Category = "XP", meta = (ClampMin = "1"))
	float XPBase;

	UPROPERTY(Config, EditAnywhere, Category = "XP", meta = (ClampMin = "1"))
	float XPExponent;

	UPROPERTY(Config, EditAnywhere, Category = "XP", meta = (ClampMin = "1"))
	int32 MaxLevel;

	// ---- Barème des points de glyphe (par niveau gagné) ----

	UPROPERTY(Config, EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	int32 GlyphPointsLevels2To20;

	UPROPERTY(Config, EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	int32 GlyphPointsLevels21To50;

	UPROPERTY(Config, EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	int32 GlyphPointsLevels51To80;

	/** Points d'Éveil par niveau gagné. */
	UPROPERTY(Config, EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	int32 AwakeningPointsPerLevel;

	/** Points d'Éveil par boss majeur (une seule fois chacun). */
	UPROPERTY(Config, EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	int32 AwakeningPointsPerBoss;

	// ---- Boss ----

	UPROPERTY(Config, EditAnywhere, Category = "Bosses", meta = (ClampMin = "0"))
	int64 GuardianXP;

	UPROPERTY(Config, EditAnywhere, Category = "Bosses", meta = (ClampMin = "0"))
	int64 SeraphXP;

	UPROPERTY(Config, EditAnywhere, Category = "Bosses", meta = (ClampMin = "0"))
	int64 DragonXP;

	/** Rayon (cm) autour du boss à sa mort pour être participant. */
	UPROPERTY(Config, EditAnywhere, Category = "Bosses", meta = (ClampMin = "0"))
	float ParticipantRadius;

	// ---- Calculs ----

	/** XP nécessaire pour passer du niveau Level au suivant. */
	int64 GetXPForNextLevel(int32 Level) const;

	/** XP cumulée nécessaire pour atteindre le niveau Level (niveau 1 = 0). */
	int64 GetCumulativeXPForLevel(int32 Level) const;

	/** Points de glyphe gagnés en atteignant le niveau Level (0 pour le niveau 1). */
	int32 GetGlyphPointsForLevel(int32 Level) const;
};
