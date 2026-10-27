// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TimerHandle.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBHitReceiver.h"
#include "EMBSeraphBoss.h"
#include "EMBSeraphShadow.generated.h"

class UCapsuleComponent;
class USkeletalMeshComponent;

/**
 * Ombre jumelle de Nyxariel (attaque Ombres jumelles, phase 2).
 *
 *  - Acteur répliqué léger : un capsule de requête (sensible au seul canal de mêlée du joueur), un mesh sans collision,
 *    aucun Tick sauf pendant un dash, aucune IA, aucune statistique.
 *  - Un seul coup d'un joueur la dissipe (IEMBHitReceiver). Durée de vie 20 s, puis dissipation d'elle-même.
 *  - Elle rejoue les événements dommageables du boss (FEMBSeraphEcho, transmis par BroadcastShadowEcho) avec 0,3 s de
 *    retard et à 30 % des dégâts, depuis sa propre position, vers le point visé par le boss au moment de l'événement :
 *    dash de l'Estoc et de la Feinte, éventail de plumes, lance, Halo noir, Ciseaux d'ailes, impact des plongeons
 *    (au point marqué par le boss). Toute la résolution passe par l'API du boss (ApplyAreaDamage, LaunchFeatherFan...) :
 *    aucune règle de dégâts n'est dupliquée ici.
 *
 * Réseau : le serveur décide (dash, dégâts, dissipation). Le mouvement n'est PAS répliqué : un dash est un aller-retour
 * purement cosmétique (Multicast_StartDash non fiable) qui se termine toujours à la position d'origine ; un message perdu
 * ne désynchronise donc jamais la position d'un clone. Seule la dissipation est répliquée (RepNotify).
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBSeraphShadow : public AActor, public IEMBHitReceiver
{
	GENERATED_BODY()

public:
	AEMBSeraphShadow();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Serveur. Tout coup d'un joueur dissipe l'ombre ; les autres sources sont ignorées. */
	virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) override;

	/** Serveur. Programme le rejeu de l'événement dans EchoDelay secondes. */
	void QueueEcho(const FEMBSeraphEcho& Echo);

	/** Serveur. Dissipe l'ombre (idempotent) : collision coupée, RepNotify, destruction après un court délai. */
	void Dissipate();

	UFUNCTION(BlueprintPure, Category = "Emberfall|Seraph")
	bool IsDissipated() const { return bDissipated; }

protected:
	/** Mise en scène : l'ombre rejoue une attaque (tous les clients). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Seraph", meta = (DisplayName = "On Shadow Echo"))
	void BP_OnEcho(FGameplayTag AttackTag, FVector AimPoint);

	/** Mise en scène : l'ombre se dissipe (fumée, fondu). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Seraph", meta = (DisplayName = "On Shadow Dissipated"))
	void BP_OnDissipated();

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayEcho(FGameplayTag AttackTag, FVector AimPoint);

	/** Aller-retour cosmétique du dash (tous les clients). Se termine toujours à la position d'origine. */
	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_StartDash(FVector Start, FVector End, float Duration);

	UFUNCTION()
	void OnRep_Dissipated();

	/** Capsule de requête : réagit seulement au canal de mêlée du joueur (EMBMeleeHit) en Overlap. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Seraph")
	TObjectPtr<UCapsuleComponent> Collision;

	/** Apparence (assignée dans le Blueprint). Aucune collision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Seraph")
	TObjectPtr<USkeletalMeshComponent> Mesh;

	/** Durée de vie avant dissipation spontanée (s). Référentiel : 20. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph", meta = (ClampMin = "1.0"))
	float Lifetime = EMBSeraphDefaults::ShadowLifetime;

	/** Retard de rejeu (s). Référentiel : 0,3. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph", meta = (ClampMin = "0.0"))
	float EchoDelay = EMBSeraphDefaults::ShadowEchoDelay;

	/** Fraction des dégâts du boss. Référentiel : 0,3. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DamageScale = EMBSeraphDefaults::ShadowDamageScale;

	/** Délai entre la dissipation et la destruction de l'acteur (laisse jouer l'effet visuel). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Seraph", meta = (ClampMin = "0.05"))
	float DissipateLingerTime = 0.6f;

private:
	void ExecuteEcho(FEMBSeraphEcho Echo);
	void StartDash(const FEMBBossAttackRow& Row, const FVector& AimPoint);
	void FinishDash();
	AEMBSeraphBoss* GetBoss() const;

	UPROPERTY(ReplicatedUsing = OnRep_Dissipated)
	bool bDissipated = false;

	// --- Dash (cosmétique sur tous, résolution des dégâts sur le serveur) ---
	bool bDashing = false;
	FVector HomeLocation = FVector::ZeroVector;
	FVector DashStart = FVector::ZeroVector;
	FVector DashEnd = FVector::ZeroVector;
	float DashDuration = EMBSeraphDefaults::DashDuration;
	float DashElapsed = 0.f;
	float DashContactRadius = EMBSeraphDefaults::DashContactRadius;

	// --- Dash : état serveur ---
	FEMBHitData DashHit;
	TSet<TWeakObjectPtr<AActor>> DashAlreadyHit;
	FTimerHandle LifetimeTimerHandle;
};
