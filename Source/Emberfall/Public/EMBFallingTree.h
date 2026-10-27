// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBFallingTree.generated.h"

class UStaticMeshComponent;
class UStaticMesh;

/** Paramètres de chute envoyés une seule fois dans le bunch initial de l'acteur. */
USTRUCT()
struct FEMBFallingTreeInit
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	/** Direction horizontale de la chute (unitaire). */
	UPROPERTY()
	FVector_NetQuantizeNormal FallDirection = FVector_NetQuantizeNormal(1.f, 0.f, 0.f);

	/** Graine du petit écart aléatoire de direction : identique sur toutes les machines. */
	UPROPERTY()
	uint8 Seed = 0;

	UPROPERTY()
	bool bValid = false;
};

/**
 * Arbre abattu : acteur de remplacement d'un AEMBResourceNode de type Tree épuisé (référentiel §13).
 *
 * Le mesh est simulé physiquement (Chaos), une impulsion au sommet le fait basculer dans la direction opposée au dernier
 * coup (vers le bûcheron). Pendant la chute, un pawn touché subit 150 dégâts et 60 de poise, une seule fois par pawn
 * (UEMBCombatComponent::ApplyHitToActor, serveur uniquement). Après 5 s la physique est mise en sommeil, l'acteur
 * disparaît à 10 s. Le plafond (3 par joueur, 12 au total) est appliqué par UEMBResourceNodeSubsystem::SpawnFallingTree.
 *
 * COÛT et RÉSEAU : une simulation Chaos de corps rigide dure ~10 s et reste bornée à 12 acteurs. La réplication du
 * mouvement physique (ReplicatedMovement + resynchronisation à chaque mise à jour) serait la part la plus chère :
 * elle est DÉSACTIVÉE. Le serveur ne réplique qu'une fois (FallInit : mesh, direction, graine) ; chaque machine simule
 * localement la même impulsion. Les trajectoires peuvent différer légèrement d'un client à l'autre (pas de
 * déterminisme Chaos garanti) : c'est purement visuel, seule la simulation du serveur inflige des dégâts.
 *
 * Prérequis d'asset : le mesh d'arbre doit porter une collision SIMPLE (hulls convexes) ; un mesh en collision
 * complexe seule ne peut pas être simulé.
 */
UCLASS(Blueprintable)
class EMBERFALL_API AEMBFallingTree : public AActor
{
	GENERATED_BODY()

public:
	AEMBFallingTree();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	/** Serveur, AVANT FinishSpawning : mesh de l'arbre, direction horizontale de chute et graine. */
	void InitFall(UStaticMesh* InMesh, const FVector& InFallDirection, int32 InSeed);

protected:
	UFUNCTION()
	void OnRep_FallInit();

	UFUNCTION()
	void OnMeshHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, FVector NormalImpulse, const FHitResult& Hit);

	void StartSimulation();
	void PutToSleep();
	void EndDamageWindow();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Emberfall|Harvest")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_FallInit)
	FEMBFallingTreeInit FallInit;

private:
	bool bSimulationStarted = false;
	bool bDamageWindowOpen = false;

	/** Pawns déjà touchés (serveur) : un seul coup par pawn. */
	TSet<TWeakObjectPtr<AActor>> DamagedActors;

	FTimerHandle SleepTimerHandle;
	FTimerHandle DamageWindowTimerHandle;
};
