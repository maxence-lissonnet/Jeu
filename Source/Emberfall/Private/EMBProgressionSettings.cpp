// Copyright ADWEB Conseil. Tous droits réservés.

#include "EMBProgressionSettings.h"

UEMBProgressionSettings::UEMBProgressionSettings()
{
	CategoryName = TEXT("Game");

	GlyphTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_Glyphs.DT_Glyphs")));
	SkillTable = TSoftObjectPtr<UDataTable>(FSoftObjectPath(TEXT("/Game/Data/DT_SkillNodes.DT_SkillNodes")));

	SecretGlyphMode = EEMBSecretGlyphMode::RevealOnly;
	DeathSaveBehavior = EEMBDeathSaveBehavior::RespawnAtSafePoint;

	XPBase = 150.f;
	XPExponent = 1.5f;
	MaxLevel = 80;

	GlyphPointsLevels2To20 = 4;
	GlyphPointsLevels21To50 = 6;
	GlyphPointsLevels51To80 = 8;
	AwakeningPointsPerLevel = 1;
	AwakeningPointsPerBoss = 1;

	GuardianXP = 6000;
	SeraphXP = 15000;
	DragonXP = 60000;
	ParticipantRadius = 10000.f;
}

int64 UEMBProgressionSettings::GetXPForNextLevel(int32 Level) const
{
	if (Level < 1)
	{
		return 0;
	}
	// round(150 x L^1,5) : L=1 -> 150 ; L=20 -> 13 416.
	return static_cast<int64>(FMath::RoundToDouble(static_cast<double>(XPBase) * FMath::Pow(static_cast<double>(Level), static_cast<double>(XPExponent))));
}

int64 UEMBProgressionSettings::GetCumulativeXPForLevel(int32 Level) const
{
	int64 Total = 0;
	const int32 Target = FMath::Min(Level, MaxLevel);
	for (int32 L = 1; L < Target; ++L)
	{
		Total += GetXPForNextLevel(L);
	}
	return Total;
}

int32 UEMBProgressionSettings::GetGlyphPointsForLevel(int32 Level) const
{
	if (Level < 2)
	{
		return 0;
	}
	if (Level <= 20)
	{
		return GlyphPointsLevels2To20;
	}
	if (Level <= 50)
	{
		return GlyphPointsLevels21To50;
	}
	return GlyphPointsLevels51To80;
}
