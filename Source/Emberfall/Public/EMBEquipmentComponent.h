// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "EMBCombatTypes.h"
#include "EMBItemTypes.h"
#include "EMBEquipmentComponent.generated.h"

class UDataTable;
class UEMBCombatComponent;
class UEMBStatsComponent;
class UEMBSurvivalComponent;

/** Le sac ou les emplacements d'équipement ont changé (serveur, ou client propriétaire après réplication) : rafraîchit l'interface. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FEMBOnEquipmentChanged);

/** Apparence visible par tous : arme/outil en main droite et bouclier (NAME_None = rien). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnVisibleEquipmentChanged, FName, MainHandItemId, FName, OffHandItemId);

/**
 * Objets durables du joueur : sac d'instances (40 maximum) et 6 emplacements (référentiel section 14).
 *
 * ARCHITECTURE RÉSEAU
 *  - Serveur autoritaire : le client n'envoie que RequestEquip(index du sac) / RequestUnequip(emplacement), RPC
 *    Server validés ; tout le reste (durabilité, stats) est calculé côté serveur.
 *  - Sac et emplacements sont répliqués au seul propriétaire (COND_OwnerOnly). Seuls les identifiants de l'arme et
 *    du bouclier visibles sont répliqués à tous, pour l'habillage des autres joueurs.
 *  - Les stats dérivées sont POUSSÉES vers UEMBCombatComponent (SetEquipmentCombatStats), UEMBStatsComponent
 *    (SetEquipmentPoiseBonus) et UEMBSurvivalComponent (SetInsulation), uniquement quand elles changent, plus un
 *    envoi forcé au BeginPlay (et un second au tick suivant, l'ordre d'initialisation des composants n'étant pas garanti).
 *
 * DURABILITÉ (serveur)
 *  - Arme/outil : -1 par coup porté (OnHitLandedServer : Hit, Killed, Blocked, GuardBroken) ou par coup de récolte
 *    (NotifyToolUsed). Bouclier : -1 par coup bloqué. Armures : -1 par coup subi (Hit, Killed) hors dégâts continus,
 *    sur chaque pièce portée. À 0 l'objet est CASSÉ : il reste équipé mais n'a plus aucun effet.
 *
 * Table : DT_Items (FEMBItemRow), chemin lu dans la configuration (section [/Script/Emberfall.EMBEquipmentComponent] de DefaultGame.ini, ajoutée par le chef d'équipe) ; défaut /Game/Data/DT_Items.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent), Config = Game)
class EMBERFALL_API UEMBEquipmentComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBEquipmentComponent();

	// ---- Contrat (section 12) ----

	/** Outil tenu en main droite. Arme, mains nues : None / tier 0 / 5 dégâts. Outil cassé : idem avec bBroken = vrai. */
	UFUNCTION(BlueprintPure, Category = "Equipment")
	FEMBEquippedToolInfo GetEquippedTool() const;

	/**
	 * Serveur : -1 de durabilité pour l'objet en main droite. La récolte ne l'appelle plus : l'usure d'un outil
	 * passe par OnHitLandedServer. Reste disponible pour tout autre usage. Sans effet à mains nues ou si l'objet est cassé.
	 */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	void NotifyToolUsed();

	/** Poids du sac d'objets + des objets équipés (ajouté à l'encombrement par le personnage). */
	UFUNCTION(BlueprintPure, Category = "Equipment")
	float GetTotalWeight() const;

	/** Serveur : ajoute un objet au sac. Durabilité -1 (ou non finie) = neuf. Faux si la ligne est inconnue ou si le sac est plein. */
	UFUNCTION(BlueprintCallable, Category = "Equipment")
	bool AddItemInstance(FName ItemId, float Durability = -1.f);

	/**
	 * Client propriétaire ou serveur : équipe l'objet du sac à cet index (échange avec l'occupant de l'emplacement).
	 * ItemId doit correspondre à l'objet présent à cet index (le sac a pu changer entre-temps), sinon la demande est ignorée.
	 * Refusé si le combat n'est pas en Idle.
	 */
	UFUNCTION(BlueprintCallable, Server, Reliable, WithValidation, Category = "Equipment")
	void RequestEquip(int32 BagIndex, FName ItemId);

	/** Client propriétaire ou serveur : range l'objet de l'emplacement dans le sac (refusé si le sac est plein ou si le combat n'est pas en Idle). */
	UFUNCTION(BlueprintCallable, Server, Reliable, WithValidation, Category = "Equipment")
	void RequestUnequip(EEMBEquipSlot Slot);

	/** Sauvegarde. Utilisable côté serveur. */
	void ExportSaveData(FEMBEquipmentSaveData& Out) const;

	/** Serveur. Import BORNÉ : identifiants inconnus ignorés, durabilité bornée, sac limité, un objet par emplacement. */
	void ImportSaveData(const FEMBEquipmentSaveData& In);

	// ---- Utilisé par l'artisanat et l'interface ----

	/** Serveur : restaure la durabilité de la première instance cassée (sac puis équipement), sinon de la première abîmée. Faux si aucune. */
	bool RepairFirstInstance(FName ItemId);

	/** Nombre d'instances de cet objet dont la durabilité est inférieure au maximum (sac + équipement). */
	UFUNCTION(BlueprintPure, Category = "Equipment")
	int32 CountDamagedInstances(FName ItemId) const;

	UFUNCTION(BlueprintPure, Category = "Equipment")
	int32 GetBagFreeSlots() const;

	UFUNCTION(BlueprintPure, Category = "Equipment")
	TArray<FEMBItemInstance> GetBagItems() const { return Bag; }

	/** Objet de l'emplacement (ItemId vide si rien). */
	UFUNCTION(BlueprintPure, Category = "Equipment")
	FEMBItemInstance GetEquippedItem(EEMBEquipSlot Slot) const;

	/** Vrai si la durabilité est épuisée (objet sans effet). Faux pour un objet inconnu ou inusable. */
	UFUNCTION(BlueprintPure, Category = "Equipment")
	bool IsInstanceBroken(const FEMBItemInstance& Instance) const;

	/** Ligne DT_Items, ou nullptr (table non chargée ou identifiant inconnu). Le pointeur est valide tant que la table l'est. */
	const FEMBItemRow* FindItemRow(FName ItemId) const;

	/**
	 * Serveur : recalcule les stats dérivées et les pousse au combat, aux stats et à la survie (seulement ce qui a
	 * changé, ou tout si bForce). À appeler par le personnage à la possession s'il le souhaite.
	 */
	void RefreshDerivedStats(bool bForce = false);

	// ---- Interface ----

	UPROPERTY(BlueprintAssignable, Category = "Equipment")
	FEMBOnEquipmentChanged OnEquipmentChanged;

	UPROPERTY(BlueprintAssignable, Category = "Equipment")
	FEMBOnVisibleEquipmentChanged OnVisibleEquipmentChanged;

	/** DataTable des objets (lignes FEMBItemRow). Valeur de configuration (DefaultGame.ini, section de la classe) ; défaut /Game/Data/DT_Items. */
	UPROPERTY(Config, BlueprintReadOnly, Category = "Equipment")
	TSoftObjectPtr<UDataTable> ItemTable;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Bag();

	UFUNCTION()
	void OnRep_Equipped();

	UFUNCTION()
	void OnRep_Visible();

	/** Abonné à UEMBCombatComponent::OnHitLandedServer (serveur) : usure de l'arme ou de l'outil. */
	UFUNCTION()
	void HandleHitLanded(AActor* Target, EEMBHitResult Result);

	/** Abonné à UEMBCombatComponent::OnHitReceived (serveur) : usure du bouclier ou des armures. */
	UFUNCTION()
	void HandleHitReceived(const FEMBHitData& Hit, EEMBHitResult Result);

private:
	bool HasAuthorityOwner() const;
	void EnsureItemTable() const;

	/** Limite de fréquence des RPC (~10/s) : faux = excédent à ignorer. */
	bool ConsumeRpcBudget();

	/** Équiper ou déséquiper n'est permis qu'en état Idle du combat. */
	bool IsCombatIdle() const;
	void ResolveDependencies();

	/** Emplacement effectif d'un objet d'après son type (nullptr-safe : None si incohérent). */
	static EEMBEquipSlot ResolveSlot(const FEMBItemRow& Row);
	static int32 SlotToIndex(EEMBEquipSlot Slot);

	/** Ligne d'un emplacement si l'objet est présent ET en état de servir (non cassé), sinon nullptr. */
	const FEMBItemRow* GetActiveRow(EEMBEquipSlot Slot) const;

	/** Retire de la durabilité à un emplacement ; renvoie vrai si l'objet vient de se casser. */
	bool WearSlot(int32 SlotIndex, float Amount, bool& bOutChanged);

	FEMBEquipmentCombatStats ComputeDerivedStats(float& OutPoiseBonus, float& OutInsulation) const;

	/** Diffuse OnEquipmentChanged et met à jour l'apparence répliquée. */
	void NotifyChanged();
	void UpdateVisibleIds();

	/** Ramène une instance lue (sauvegarde) dans des bornes valides ; faux si l'objet est inconnu. */
	bool SanitizeInstance(const FEMBItemInstance& In, FEMBItemInstance& Out) const;

	void HandleDeferredPush();

	/** Chargée à la demande (FindItemRow est const) : EnsureItemTable passe par un const_cast. */
	UPROPERTY(Transient)
	TObjectPtr<UDataTable> LoadedItems;

	UPROPERTY(ReplicatedUsing = OnRep_Bag)
	TArray<FEMBItemInstance> Bag;

	/** Six emplacements, indexés par EEMBEquipSlot - 1 (MainHand, OffHand, Head, Chest, Hands, Legs). */
	UPROPERTY(ReplicatedUsing = OnRep_Equipped)
	TArray<FEMBItemInstance> Equipped;

	UPROPERTY(ReplicatedUsing = OnRep_Visible)
	FName VisibleMainHandId = NAME_None;

	UPROPERTY(ReplicatedUsing = OnRep_Visible)
	FName VisibleOffHandId = NAME_None;

	UPROPERTY(Transient)
	TObjectPtr<UEMBCombatComponent> Combat;

	UPROPERTY(Transient)
	TObjectPtr<UEMBStatsComponent> Stats;

	UPROPERTY(Transient)
	TObjectPtr<UEMBSurvivalComponent> Survival;

	bool bEventsBound = false;

	// Fenêtre de limitation des RPC (serveur).
	double RpcWindowStart = 0.0;
	int32 RpcWindowCount = 0;

	// Dernières valeurs poussées (serveur) : on ne repousse que ce qui change.
	FEMBEquipmentCombatStats PushedCombat;
	float PushedPoise = 0.f;
	float PushedInsulation = 0.f;
	bool bCombatPushed = false;
	bool bPoisePushed = false;
	bool bInsulationPushed = false;
};
