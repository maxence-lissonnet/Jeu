// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/TimerHandle.h"
#include "EMBCombatTypes.h"
#include "EMBProjectile.generated.h"

class UProjectileMovementComponent;
class USphereComponent;

/** Canal d'objet dédié des projectiles : EMBProjectile = ECC_GameTraceChannel3 (Config/DefaultEngine.ini). */
namespace EMBProjectileDefaults
{
	constexpr ECollisionChannel ObjectChannel = ECC_GameTraceChannel3;
}

/** Apparence du projectile (choisit le VFX / le mesh dans le Blueprint). */
UENUM(BlueprintType)
enum class EEMBProjectileStyle : uint8
{
	Feather UMETA(DisplayName = "Plume"),
	Lance   UMETA(DisplayName = "Lance")
};

/**
 * État réseau d'un projectile : tout ce dont un client a besoin pour simuler la trajectoire localement.
 * Le mouvement n'est PAS répliqué (bReplicateMovement = false) : le serveur publie une seule fois l'origine et la
 * vitesse, chaque client déroule ensuite la ligne droite avec son propre UProjectileMovementComponent. Pour une
 * rafale de 24 plumes, cela remplace 24 flux de positions à 30 Hz par 24 messages uniques.
 * Sequence change à chaque (re)lancement ou renvoi : c'est lui qui déclenche le RepNotify, même si le projectile
 * revient d'une session précédente avec les mêmes valeurs.
 */
USTRUCT()
struct FEMBProjectileNetState
{
	GENERATED_BODY()

	UPROPERTY()
	bool bActive = false;

	UPROPERTY()
	bool bReflected = false;

	UPROPERTY()
	uint8 Sequence = 0;

	UPROPERTY()
	EEMBProjectileStyle Style = EEMBProjectileStyle::Feather;

	UPROPERTY()
	FVector Origin = FVector::ZeroVector;

	UPROPERTY()
	FVector Velocity = FVector::ZeroVector;

	/** Heure serveur (AGameStateBase::GetServerWorldTimeSeconds) du lancement : sert au rattrapage de latence. */
	UPROPERTY()
	double ServerTime = 0.0;
};

/** Paramètres d'un tir (côté serveur, aucun besoin de réplication : structure C++ simple). */
struct FEMBProjectileLaunchParams
{
	FVector Origin = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	float Speed = 3000.f;
	float CollisionRadius = 25.f;
	float Lifetime = 2.f;
	EEMBProjectileStyle Style = EEMBProjectileStyle::Feather;

	/** Gabarit du coup : Instigator = le boss, dégâts, poise, Corruption, parabilité. */
	FEMBHitData Hit;

	/** Parade parfaite = renvoi vers l'instigateur (lance de Nyxariel). */
	bool bReflectOnPerfectParry = false;
	float ReflectDamage = 0.f;
	float ReflectSpeed = 0.f;
};

/**
 * Projectile répliqué simple des boss (plumes et lance de Nyxariel).
 *
 *  - Mouvement : UProjectileMovementComponent en ligne droite (gravité 0), sous-pas forcés à 60 Hz pour qu'un
 *    projectile à 4 000 cm/s ne traverse jamais la capsule d'un joueur, même sur un serveur à 30 Hz.
 *  - Collision : sphère. Pawns en Overlap (la résolution du coup est faite par UEMBCombatComponent::ApplyHitToActor,
 *    qui décide esquive / garde / parade), décor en Block (un pilier ou une cloche arrête le projectile).
 *  - Parade parfaite (résultat Parried) sur un projectile bReflectOnPerfectParry : il repart vers son instigateur
 *    (le boss) à ReflectSpeed et lui inflige ReflectDamage (300) à l'arrivée ; l'instigateur du coup devient le joueur
 *    qui a paré (menace, XP).
 *  - Esquive (résultat Dodged) : le projectile continue sa route sans toucher ce joueur une seconde fois.
 *  - Durée de vie : timer serveur, puis retour au pool.
 *
 * POOL. L'acteur est conçu pour être réutilisé (AEMBSeraphBoss tient le pool, voir AcquireProjectile) : une rafale de
 * 24 plumes toutes les 10 s créerait sinon 24 SpawnActor + 24 destructions répliquées (ouverture et fermeture de
 * canaux de réplication, pression sur le GC) à chaque rafale, doublées par les clones. Avec le pool, le coût d'une
 * rafale se limite à 24 téléportations + 24 mises à jour d'état ; un projectile inactif est caché, sans collision, sans
 * mouvement, et ne coûte plus qu'une comparaison de propriétés. Amélioration possible (non faite, à valider en jeu) :
 * mise en sommeil réseau (SetNetDormancy / FlushNetDormancy) des projectiles inactifs.
 *
 * Réseau : bReplicates, pas de réplication de mouvement, une seule propriété répliquée (FEMBProjectileNetState).
 * Les dégâts ne sont JAMAIS résolus sur les clients.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBProjectile : public AActor
{
	GENERATED_BODY()

public:
	AEMBProjectile();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Serveur. Active et lance le projectile (instance neuve ou recyclée). */
	void Launch(const FEMBProjectileLaunchParams& Params);

	/** Serveur. Désactive le projectile et le rend au pool (idempotent). */
	void ReturnToPool();

	/** Faux : disponible dans le pool. Lisible sur le serveur et sur les clients. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Projectile")
	bool IsProjectileActive() const { return NetState.bActive; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Projectile")
	bool IsReflected() const { return NetState.bReflected; }

protected:
	/** Mise en scène : le projectile vient d'être lancé (serveur et clients). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Projectile", meta = (DisplayName = "On Projectile Launched"))
	void BP_OnLaunched(EEMBProjectileStyle Style);

	/** Mise en scène : le projectile a été renvoyé par une parade parfaite (serveur et clients). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Projectile", meta = (DisplayName = "On Projectile Reflected"))
	void BP_OnReflected();

	/** Mise en scène : fin de vie (impact, expiration, touche). Serveur et clients. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Emberfall|Projectile", meta = (DisplayName = "On Projectile Ended"))
	void BP_OnEnded();

	UFUNCTION()
	void OnRep_NetState(const FEMBProjectileNetState& OldState);

	UFUNCTION()
	void HandleBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
		int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void HandleProjectileStop(const FHitResult& ImpactResult);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Projectile")
	TObjectPtr<USphereComponent> Collision;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Projectile")
	TObjectPtr<UProjectileMovementComponent> Movement;

	/** Rattrapage de latence maximal appliqué côté client au lancement (s) : évite un projectile "en retard" visible. */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Projectile", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float MaxClientCatchUpTime = 0.25f;

	/** Parade parfaite = renvoi (copie de travail ; écrasée par les paramètres de tir). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Projectile")
	bool bReflectOnPerfectParry = false;

	/** Dégâts infligés à l'instigateur par un projectile renvoyé (référentiel : 300). */
	UPROPERTY(EditDefaultsOnly, Category = "Emberfall|Projectile", meta = (ClampMin = "0.0"))
	float ReflectDamage = 300.f;

private:
	/** Place le projectile, active collision et mouvement. Commun au tir serveur, au renvoi et à la simulation client. */
	void StartMotion(const FVector& InOrigin, const FVector& InVelocity);
	void StopMotion();
	void SetVisualsVisible(bool bVisible);
	void NotifyLaunched();
	void NotifyReflected();
	void NotifyEnded();

	/** Publie l'état aux clients (serveur) : incrémente la séquence. */
	void PublishState(bool bActive, bool bReflected, const FVector& InOrigin, const FVector& InVelocity);

	void ResolveHitOnPlayer(APawn* Player);
	void ResolveHitOnReflectTarget(AActor* Target);
	void Reflect(AActor* Parrier);
	void ArmLifetimeTimer(float Seconds);

	double GetSyncedTime() const;

	UPROPERTY(ReplicatedUsing = OnRep_NetState)
	FEMBProjectileNetState NetState;

	// --- État serveur ---
	FEMBHitData HitData;
	float ReflectSpeed = 0.f;
	float CurrentSpeed = 0.f;
	EEMBProjectileStyle CurrentStyle = EEMBProjectileStyle::Feather;
	TWeakObjectPtr<AActor> OriginBoss;
	TWeakObjectPtr<AActor> ReflectTarget;
	TSet<TWeakObjectPtr<AActor>> ResolvedActors;
	FTimerHandle LifetimeTimer;
};
