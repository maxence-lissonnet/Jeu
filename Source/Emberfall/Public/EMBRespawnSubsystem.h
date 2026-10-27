// Emberfall - points de réapparition et sacs de mort (Dev B, référentiel section 9).

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "EMBInventoryComponent.h"
#include "EMBRespawnSubsystem.generated.h"

class AController;
class APlayerState;
class AEMBBuildablePiece;
class AEMBDeathBag;

/**
 * Réapparition d'un monde : liaisons joueur -> point de réapparition, et sacs de mort. Serveur seulement pour toute
 * écriture ; les clients n'utilisent pas ce sous-système.
 *
 * Liaisons : clé du joueur -> pièce (pointeur faible). Une pièce détruite ou effondrée invalide la liaison toute seule
 * (le pointeur faible tombe, ou la pièce est marquée effondrée) : GetRespawnTransform renvoie alors false et le
 * GameMode retombe sur le PlayerStart. À la sauvegarde, la liaison est convertie en SaveId de pièce.
 *
 * Clé d'un joueur : l'identifiant stable de la sauvegarde pour un joueur persisté ; "Session_<PlayerId>" pour un joueur
 * dont les données ne sont pas persistées (identifiant en doublon). Un sac ou une liaison de ce dernier n'est jamais sauvegardé.
 */
UCLASS(Config = Game)
class EMBERFALL_API UEMBRespawnSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UEMBRespawnSubsystem* Get(const UObject* WorldContextObject);

	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// ----- Signatures figées (référentiel 9.3) -----

	/** Serveur : lie le joueur à ce point de réapparition (le dernier lien remplace le précédent). Refusé si la pièce n'en est pas un. */
	void BindRespawnPoint(APlayerState* Player, AEMBBuildablePiece* Piece);

	/**
	 * Position de réapparition : point lié valide, décalé de 150 cm devant la pièce et posé sur le sol.
	 * @return false si aucun point lié valide (le GameMode utilise alors un PlayerStart).
	 */
	bool GetRespawnTransform(AController* Controller, FTransform& OutTransform) const;

	/**
	 * Serveur : vide l'inventaire du pawn mort dans un sac. À appeler à la RÉAPPARITION (pas si le joueur est réanimé).
	 * Un seul sac par joueur : l'ancien sac et son contenu sont détruits. Position : lieu de la mort projeté au sol
	 * (trace de 500 cm vers le bas) ; si le sol est absent (chute, vide), dernier point sûr relevé par la sauvegarde ;
	 * à défaut, lieu de la mort tel quel. Limite connue : une mort dans la lave sur un sol existant dépose le sac sur
	 * ce sol (la lave n'est pas détectable ici) ; Dev A peut passer un autre lieu en entrée s'il connaît la cause.
	 */
	void DropDeathBag(AController* Controller, const FVector& DeathLocation);

	// ----- Compléments -----

	/** Clé de sauvegarde / de sac du joueur. Vide si le contrôleur n'a pas de PlayerState exploitable. */
	FString GetPlayerKey(const AController* Controller) const;

	/** Pièce actuellement liée à cette clé, ou nullptr si aucune liaison valide (pièce détruite, effondrée, plus un point de réapparition). */
	AEMBBuildablePiece* GetBoundPiece(const FString& PlayerKey) const;

	/** Chargement d'une sauvegarde : rétablit une liaison sans passer par une interaction. */
	void RestoreBinding(const FString& PlayerKey, AEMBBuildablePiece* Piece);

	/** Chargement d'une sauvegarde : recrée un sac. Remplace un éventuel sac existant du même propriétaire. */
	void RestoreBag(const FString& OwnerKey, const FTransform& Transform, const TArray<FEMBResourceStack>& Contents);

	/** Sacs à sauvegarder (hors joueurs non persistés). */
	void GetPersistentBags(TArray<const AEMBDeathBag*>& OutBags) const;

	/** Un joueur (re)connecté : son sac éventuel reçoit son PlayerId de session (pour l'invite d'interaction côté client). */
	void NotifyPlayerPresent(AController* Controller);

	/**
	 * Serveur : autorise une réduction de Corruption par le sommeil, au plus une fois toutes les
	 * EMBBuildingDefaults::BedSleepCooldownSeconds (120 s) PAR JOUEUR (et non par lit, sinon deux lits la doubleraient).
	 * Enregistre l'instant si elle est accordée. La liaison au lit, elle, reste instantanée et illimitée.
	 */
	bool TryConsumeSleepBenefit(const AController* Controller);

	/** Appelé par le sac en EndPlay : le retire du registre. */
	void NotifyBagDestroyed(AEMBDeathBag* Bag);

	/** Classe de sac (Blueprint dérivé de AEMBDeathBag avec un mesh). Vide : AEMBDeathBag natif. Réglable dans DefaultGame.ini. */
	UPROPERTY(Config)
	TSoftClassPtr<AEMBDeathBag> DeathBagClass;

private:
	bool IsAuthority() const;
	AEMBDeathBag* SpawnBag(const FString& OwnerKey, const FTransform& Transform, int32 OwnerPlayerId, bool bPersistent, const TArray<FEMBResourceStack>& Contents);
	FVector ResolveBagLocation(const FString& OwnerKey, const FVector& DeathLocation, const AActor* IgnoredActor) const;

	TMap<FString, TWeakObjectPtr<AEMBBuildablePiece>> Bindings;
	TMap<FString, TWeakObjectPtr<AEMBDeathBag>> BagsByOwner;

	/** Dernier bénéfice de sommeil accordé, par clé de joueur (secondes de jeu du monde). */
	TMap<FString, double> LastSleepBenefitTime;
};
