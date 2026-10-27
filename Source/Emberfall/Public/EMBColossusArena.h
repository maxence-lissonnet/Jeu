// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBHitReceiver.h"
#include "EMBCombatTypes.h"
#include "EMBColossusArena.generated.h"

class AEMBColossusBoss;
class AEMBObelisk;
class UStaticMeshComponent;

/** Un obélisque vient d'être détruit (serveur et clients). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnObeliskDestroyed, AEMBObelisk*, Obelisk);

/**
 * Obélisque runique de l'arène du Colosse : 500 PV, endommageable par les joueurs (et par la charge du Colosse).
 * Implémente IEMBHitReceiver : UEMBCombatComponent::ApplyHitToActor lui route donc les coups sans autre configuration.
 *
 * Intact = PV > 0. Allumé en phase 2 (par l'arène) ; seul un obélisque intact alimente la Résonance du Colosse.
 * Réseau : PV, état allumé et état détruit sont répliqués (RepNotify -> événements Blueprint de mise en scène).
 * Collision : profil BlockAll (WorldStatic) pour arrêter la charge du Colosse ; il doit aussi recevoir les coups des
 * joueurs (canal de détection de coup de l'arme à bloquer ou à recouvrir).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBObelisk : public AActor, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBObelisk();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	/** Serveur uniquement. Retire Hit.Damage aux PV ; à 0 l'obélisque est détruit (collision coupée, caché par le Blueprint). */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	UFUNCTION(BlueprintPure, Category = "Emberfall|Obelisk")
	bool IsIntact() const { return Health > 0.f; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Obelisk")
	bool IsLit() const { return bLit; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Obelisk")
	float GetHealth() const { return Health; }

	/** Allume ou éteint l'obélisque (phase 2 : allumé). Serveur ; les clients suivent par RepNotify. */
	void SetLit(bool bNewLit);

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Obelisk")
	FEMBOnObeliskDestroyed OnObeliskDestroyed;

protected:
	/** Mise en scène : changement d'état allumé (runes, lumière). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Obelisk", meta = (DisplayName = "On Lit Changed"))
	void BP_OnLitChanged(bool bNewLit);

	/** Mise en scène : coup reçu (étincelles, fissures). Serveur et clients (via RepNotify des PV). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Obelisk", meta = (DisplayName = "On Health Changed"))
	void BP_OnHealthChanged(float NewHealth, float NewMaxHealth);

	/** Mise en scène : destruction (éboulement, VFX). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Obelisk", meta = (DisplayName = "On Obelisk Destroyed"))
	void BP_OnDestroyed();

	UFUNCTION()
	void OnRep_Health(float OldHealth);

	UFUNCTION()
	void OnRep_Lit();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Obelisk")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** PV maximum (référentiel : 500). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Emberfall|Obelisk", meta = (ClampMin = "1.0"))
	float MaxHealth = 500.f;

private:
	void ApplyDestroyedState();

	UPROPERTY(ReplicatedUsing = OnRep_Health)
	float Health = 500.f;

	UPROPERTY(ReplicatedUsing = OnRep_Lit)
	bool bLit = false;

	bool bDestroyedStateApplied = false;
};

/**
 * Arène évolutive du Colosse d'Ossivaire : cour circulaire de 50 m, 4 obélisques de 500 PV (GDD §9.1).
 *
 *  - Phase 2 : les obélisques intacts s'allument (et alimentent la Résonance du Colosse).
 *  - Boss vaincu (combat ou sauvegarde) : état final calme, obélisques éteints.
 *  - Charge de bélier : IsChargeObstacle() dit quels acteurs arrêtent et étourdissent le Colosse (obélisques, murs
 *    portant le tag ChargeWallTag, ou tout décor statique si bAnyStaticActorStopsCharge).
 *
 * Comme l'arène du dragon : non répliquée ; chaque machine applique le même état déterministe à partir de la
 * phase répliquée du boss (rattrapage à la liaison pour les joueurs qui se connectent en cours de combat).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBColossusArena : public AActor
{
	GENERATED_BODY()

public:
	AEMBColossusArena();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Lie l'arène à un boss (appelé automatiquement au BeginPlay ; public pour un boss instancié plus tard). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Arena")
	void BindToBoss(AEMBColossusBoss* Boss);

	/** Nombre d'obélisques intacts (PV > 0). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Arena")
	int32 GetIntactObeliskCount() const;

	/** Cet acteur arrête-t-il la charge du Colosse (et l'étourdit) ? */
	bool IsChargeObstacle(const AActor* HitActor) const;

protected:
	/** Mise en scène (cinématique, ambiance) après l'application d'une phase : serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Arena", meta = (DisplayName = "On Arena Phase Applied"))
	void OnArenaPhaseApplied(int32 Phase);

	/** Mise en scène de l'état final calme (boss vaincu). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Arena", meta = (DisplayName = "On Arena Final Calm State"))
	void OnArenaFinalCalmState();

	UFUNCTION()
	void HandleBossPhaseChanged(int32 NewPhase, int32 OldPhase);

	UFUNCTION()
	void HandleEncounterEnded();

	/** Boss de l'arène (instance du niveau). Si vide, le premier AEMBColossusBoss du monde est utilisé. */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TSoftObjectPtr<AEMBColossusBoss> BossActor;

	/** Les 4 obélisques de la cour (instances du niveau). */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TArray<TObjectPtr<AEMBObelisk>> Obelisks;

	/** Tag d'acteur identifiant un mur / une limite d'arène qui arrête la charge. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Arena")
	FName ChargeWallTag = TEXT("ArenaWall");

	/** Vrai : tout décor statique bloquant rencontré par la charge l'arrête aussi (en plus des obélisques et des murs taggés). Faux par défaut : un sac de mort ou un volume ne doit jamais étourdir le Colosse. */
	UPROPERTY(EditAnywhere, Category = "Emberfall|Arena")
	bool bAnyStaticActorStopsCharge = false;

private:
	void TryBindToBoss();
	void EnterFinalCalmState();

	TWeakObjectPtr<AEMBColossusBoss> BoundBoss;
	int32 AppliedPhase = 0;
	bool bFinalCalm = false;

	FTimerHandle BossSearchTimerHandle;
};
