// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "EMBWeatherTypes.h"
#include "EMBWorldStateReplicator.generated.h"

/**
 * Réplicateur de l'état du monde (heure et météo), contrat 15. Spawné par UEMBWeatherSubsystem sur le serveur
 * (ou en solo), toujours pertinent pour tous les clients.
 *
 * RÉSEAU : rien n'est répliqué à l'image. On envoie
 *  - une heure de RÉFÉRENCE et le temps serveur correspondant, plus les durées du jour et de la nuit (les
 *    « vitesses ») : chaque machine recalcule l'heure localement avec le temps serveur synchronisé
 *    (AGameStateBase::GetServerWorldTimeSeconds) ;
 *  - l'état de chaque biome (état stable, état visé, début et durée de la transition) : il ne change qu'à chaque
 *    nouvelle transition, soit quelques fois par biome toutes les 5 à 10 minutes.
 *
 * Le serveur est le seul à écrire (fonctions Server*). Les clients lisent via UEMBWeatherSubsystem.
 */
UCLASS(NotBlueprintable)
class EMBERFALL_API AEMBWorldStateReplicator : public AInfo
{
	GENERATED_BODY()

public:
	AEMBWorldStateReplicator();

	/** Temps serveur synchronisé : sur le serveur, le temps monde ; sur un client, GetServerWorldTimeSeconds. */
	double GetServerTimeNow() const;

	/** Heure du jour (0-24) à l'instant ServerNow, calculée localement depuis la référence répliquée. */
	float ComputeTimeOfDay(double ServerNow) const;

	const TArray<FEMBBiomeWeatherState>& GetBiomes() const { return Biomes; }

	float GetDayDurationSeconds() const { return DayDurationSeconds; }
	float GetNightDurationSeconds() const { return NightDurationSeconds; }

	// ---- Écriture (serveur uniquement)

	/** Fixe l'heure de référence : à partir de ServerTime, le monde est à Hour. Durées bornées. */
	void ServerSetTimeReference(float Hour, double ServerTime, float InDayDurationSeconds, float InNightDurationSeconds);

	/** Remplace les états de biome (rien n'est envoyé si le contenu est identique) et force une mise à jour réseau. */
	void ServerSetBiomes(const TArray<FEMBBiomeWeatherState>& NewBiomes);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UFUNCTION() void OnRep_TimeReference();
	UFUNCTION() void OnRep_Biomes();

	/** Prévient le sous-système local pour qu'il rafraîchisse l'environnement sans attendre sa période. */
	void NotifySubsystem() const;

	UPROPERTY(ReplicatedUsing = OnRep_TimeReference) float ReferenceHour = EMBWeatherDefaults::StartHour;
	UPROPERTY(ReplicatedUsing = OnRep_TimeReference) double ReferenceServerTime = 0.0;
	UPROPERTY(ReplicatedUsing = OnRep_TimeReference) float DayDurationSeconds = EMBWeatherDefaults::DayDurationSeconds;
	UPROPERTY(ReplicatedUsing = OnRep_TimeReference) float NightDurationSeconds = EMBWeatherDefaults::NightDurationSeconds;

	UPROPERTY(ReplicatedUsing = OnRep_Biomes) TArray<FEMBBiomeWeatherState> Biomes;
};
