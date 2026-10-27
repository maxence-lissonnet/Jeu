// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TimerHandle.h"
#include "EMBHitReceiver.h"
#include "EMBCombatTypes.h"
#include "EMBSeraphArena.generated.h"

class AEMBSeraphBell;
class AEMBSeraphBoss;
class UStaticMeshComponent;

/** Une cloche vient d'être frappée par un joueur (serveur uniquement). Striker = le pawn du joueur. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnBellStruck, AEMBSeraphBell*, Bell, AActor*, Striker);

/**
 * Cloche du Clocher effondré de Vel-Morrow (GDD §9.2). Implémente IEMBHitReceiver : UEMBCombatComponent::ApplyHitToActor
 * lui route donc les coups des joueurs sans autre configuration.
 *
 * La cloche ne décide pas de l'effet : elle prévient l'arène (OnBellStruck), qui sait si Nyxariel vole. Le coup sonne
 * toujours (retour visuel et sonore), mais la cloche n'est "consommée" que si elle a effectivement étourdi le boss
 * (MarkUsed) : une cloche frappée pendant que Nyxariel est au sol reste donc utilisable.
 *
 * Collision : profil BlockAll (WorldStatic), comme les obélisques du Colosse : elle reçoit le balayage d'arme des
 * joueurs ET arrête les plumes (le joueur peut s'abriter derrière une cloche, GDD).
 * Réseau : l'état consommé / réduit au silence est répliqué (RepNotify -> événements Blueprint).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBSeraphBell : public AActor, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBSeraphBell();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Serveur uniquement. Seul un joueur la fait sonner. Retourne Hit (la cloche n'encaisse aucun dégât). */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	/** Serveur. Marque la cloche comme consommée (elle a étourdi le boss) : plus aucun effet. */
	void MarkUsed();

	/** Serveur. Réduit la cloche au silence (état final calme : boss vaincu). */
	void SetSilenced(bool bNewSilenced);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Bell")
	bool IsUsed() const { return bUsed; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Bell")
	bool IsSilenced() const { return bSilenced; }

	/** Serveur : coup de joueur reçu. L'arène s'y abonne. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Bell")
	FEMBOnBellStruck OnBellStruck;

protected:
	/** Mise en scène : la cloche sonne (tous les clients). bEffective : le coup a étourdi Nyxariel. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Bell", meta = (DisplayName = "On Bell Rung"))
	void BP_OnRung(bool bEffective);

	/** Mise en scène : changement d'état consommé (cloche fêlée, lumière éteinte). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Bell", meta = (DisplayName = "On Bell Used Changed"))
	void BP_OnUsedChanged(bool bNewUsed);

	/** Mise en scène : état final calme (boss vaincu). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Bell", meta = (DisplayName = "On Bell Silenced"))
	void BP_OnSilenced(bool bNewSilenced);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_Ring(bool bEffective);

	UFUNCTION()
	void OnRep_Used();

	UFUNCTION()
	void OnRep_Silenced();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Bell")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** Délai minimal entre deux sonneries (s) : un même balayage ne doit pas faire sonner la cloche plusieurs fois. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Bell", meta = (ClampMin = "0.0"))
	float MinRingInterval = 0.4f;

private:
	UPROPERTY(ReplicatedUsing = OnRep_Used)
	bool bUsed = false;

	UPROPERTY(ReplicatedUsing = OnRep_Silenced)
	bool bSilenced = false;

	double LastRingTime = -1000.0;
};

/**
 * Arène de Nyxariel : Clocher effondré de Vel-Morrow, plates-formes sur trois niveaux, 3 cloches (GDD §9.2).
 *
 *  - Cloche frappée par un joueur pendant que Nyxariel VOLE (et n'est ni étourdi, ni en transition, ni mort) : il est
 *    étourdi 2 s (AEMBBossBase::BeginStun, qui le fait aussi retomber au sol), une fois par cloche.
 *  - Boss vaincu (au combat, ou déjà vaincu d'après la sauvegarde : la base diffuse OnEncounterEnded dans les deux cas) :
 *    état final calme, cloches réduites au silence.
 *  - Changement de phase : événement Blueprint de mise en scène (effondrement de plates-formes, ambiance).
 *
 * Comme les arènes du dragon et du Colosse : non répliquée ; chaque machine applique le même état déterministe à partir
 * de la phase répliquée du boss (rattrapage à la liaison pour les joueurs qui se connectent en cours de combat). Seule
 * la décision d'étourdir est serveur.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBSeraphArena : public AActor
{
	GENERATED_BODY()

public:
	AEMBSeraphArena();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Lie l'arène à un boss (appelé automatiquement au BeginPlay ; public pour un boss instancié plus tard). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Arena")
	void BindToBoss(AEMBSeraphBoss* Boss);

	/** Nombre de cloches encore utilisables (non consommées). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Arena")
	int32 GetUnusedBellCount() const;

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

	UFUNCTION()
	void HandleBellStruck(AEMBSeraphBell* Bell, AActor* Striker);

	/** Boss de l'arène (instance du niveau). Si vide, le premier AEMBSeraphBoss du monde est utilisé. */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TSoftObjectPtr<AEMBSeraphBoss> BossActor;

	/** Les 3 cloches du clocher (instances du niveau). */
	UPROPERTY(EditInstanceOnly, Category = "Emberfall|Arena")
	TArray<TObjectPtr<AEMBSeraphBell>> Bells;

private:
	void TryBindToBoss();
	void EnterFinalCalmState();

	TWeakObjectPtr<AEMBSeraphBoss> BoundBoss;
	int32 AppliedPhase = 0;
	bool bFinalCalm = false;

	FTimerHandle BossSearchTimerHandle;
};
