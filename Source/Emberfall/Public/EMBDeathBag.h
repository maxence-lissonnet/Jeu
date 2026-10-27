// Emberfall - sac de mort (Dev B, référentiel section 9).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EMBInteractable.h"
#include "EMBInventoryComponent.h"
#include "EMBDeathBag.generated.h"

class USphereComponent;
class UStaticMeshComponent;

/**
 * Sac déposé à la RÉAPPARITION d'un joueur mort (pas s'il est réanimé) : il contient toutes les ressources de son
 * inventaire. Seul son propriétaire peut le ramasser.
 *
 * Collision : la racine est une sphère (rayon 60 cm) en requête seule, qui bloque le canal Visibility (traces
 * d'interaction) et chevauche les pawns. Le sac est donc ramassable même sans Blueprint ni mesh ; un mesh optionnel
 * (Blueprint dérivé) s'attache à la sphère, sans collision.
 *
 * Réseau :
 *  - Le contenu n'est PAS répliqué : aucun client n'en a besoin, et il évite d'envoyer l'inventaire de tous les joueurs.
 *  - Le propriétaire est identifié côté serveur par une clé de sauvegarde (identifiant stable). Cette clé (qui peut être
 *    un identifiant Steam/EOS) n'est JAMAIS répliquée : les clients ne reçoivent que le PlayerId de session du
 *    propriétaire, suffisant pour griser l'invite d'interaction chez les autres joueurs.
 *  - Dormance : NetDormancy vaut DORM_Initial, qui ne rend dormant qu'un acteur placé dans le niveau ; un sac créé en jeu
 *    reste donc réveillé. C'est sans conséquence : un sac ne change quasiment jamais (seule l'attribution tardive du
 *    PlayerId à la connexion du propriétaire modifie une propriété répliquée), sa fréquence de mise à jour est de 1 Hz
 *    et leur nombre reste faible (un par joueur au plus).
 */
UCLASS()
class EMBERFALL_API AEMBDeathBag : public AActor, public IEMBInteractable
{
	GENERATED_BODY()

public:
	AEMBDeathBag();

	/**
	 * Serveur, avant FinishSpawning.
	 * @param InOwnerKey clé du propriétaire (UEMBRespawnSubsystem::GetPlayerKey).
	 * @param InOwnerPlayerId APlayerState::GetPlayerId() du propriétaire s'il est connecté, sinon INDEX_NONE.
	 * @param bInPersistent faux pour un joueur dont les données ne sont pas persistées (identifiant en doublon) : le sac n'est pas sauvegardé.
	 */
	void InitializeBag(const FString& InOwnerKey, int32 InOwnerPlayerId, bool bInPersistent, const TArray<FEMBResourceStack>& InContents);

	const FString& GetOwnerKey() const { return OwnerKey; }
	bool IsPersistent() const { return bPersistent; }
	const TArray<FEMBResourceStack>& GetContents() const { return Contents; }
	int32 GetOwnerPlayerId() const { return OwnerPlayerId; }

	/** Serveur : le propriétaire se (re)connecte, on lui attribue son PlayerId de session. */
	void SetOwnerPlayerId(int32 InOwnerPlayerId);

	// ----- IEMBInteractable -----

	/** Propriétaire uniquement. Serveur : compare les clés stables ; client : compare les PlayerId répliqués. */
	virtual bool CanInteract(const APawn* InInstigator) const override;

	/**
	 * Serveur : transfère les ressources vers l'inventaire du ramasseur (aucune limite de poids en v1). Le sac n'est
	 * détruit que si TOUT a été transféré ; sinon (plafond par ressource atteint) il garde le reste.
	 */
	virtual void Interact(APawn* InInstigator) override;

	virtual FText GetInteractionText(const APawn* InInstigator) const override;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Racine : zone d'interaction. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DeathBag")
	TObjectPtr<USphereComponent> Collision;

	/** Mesh optionnel (à renseigner dans un Blueprint dérivé). Sans collision. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DeathBag")
	TObjectPtr<UStaticMeshComponent> Mesh;

	/** Distance de culling réseau (cm). */
	UPROPERTY(EditDefaultsOnly, Category = "DeathBag|Network", meta = (ClampMin = "1000"))
	float NetCullDistance = 10000.f;

private:
	/** PlayerId de session du propriétaire (INDEX_NONE si hors ligne). Seule information de propriété répliquée. */
	UPROPERTY(Replicated)
	int32 OwnerPlayerId = INDEX_NONE;

	/** Serveur seulement. */
	FString OwnerKey;
	bool bPersistent = true;
	TArray<FEMBResourceStack> Contents;
};
