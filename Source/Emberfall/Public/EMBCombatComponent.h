// Copyright ADWEB Conseil. Tous droits réservés.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/TimerHandle.h"
#include "GameplayTagContainer.h"
#include "EMBCombatTypes.h"
#include "EMBProgressionTypes.h"
#include "EMBCombatComponent.generated.h"

class UAnimInstance;
class UAnimMontage;
class UEMBStatsComponent;
class UMeshComponent;
class USceneComponent;
class USkeletalMeshComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnCombatStateChanged, EEMBCombatState, OldState, EEMBCombatState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnParrySuccess, AActor*, Attacker);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnHitReceived, const FEMBHitData&, Hit, EEMBHitResult, Result);
/** Retour de coup côté attaquant (client propriétaire) : cible touchée, résultat, point d'impact. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FEMBOnHitConfirmed, AActor*, Target, EEMBHitResult, Result, FVector, Location);

/**
 * Machine à états du combat du joueur (Idle, LightAttack, HeavyAttack, ChargedAttack, Dodge, Parry,
 * Block, Staggered, Recovery, Dead).
 *
 * ARCHITECTURE RÉSEAU
 *  - Le client propriétaire appelle HandleInput() (Enhanced Input). Il joue le montage de l'action en
 *    local par prédiction (réactivité), puis envoie Server_HandleInput (RPC Reliable).
 *  - Le SERVEUR fait tourner la FSM : validation, endurance, buffer d'input, fenêtres, détection des coups,
 *    résolution des dégâts. CurrentState est répliqué (RepNotify -> OnCombatStateChanged côté client).
 *  - Les montages sont rediffusés par un multicast Unreliable ; le propriétaire ignore le multicast si le
 *    montage tourne déjà (prédiction). Compromis assumé : pas de rollback complet. Si le serveur refuse
 *    l'action (endurance vide, état incompatible), il envoie Client_RejectPrediction qui coupe le montage
 *    prédit ; en cas de désaccord prolongé, l'état répliqué fait foi pour le gameplay et seul l'habillage
 *    visuel peut être transitoirement faux sur le client propriétaire.
 *
 * FENÊTRES D'ANIMATION
 *  - ComboWindow, IFrames et HitWindow sont des AnimNotifyState qui appellent les fonctions "notify" de ce
 *    composant. Pour que les notifies se déclenchent sur un serveur dédié, le mesh du personnage doit être
 *    animé même hors écran (voir AEMBPlayerCharacter : AlwaysTickPoseAndRefreshBones).
 *  - Sans montage assigné (prototypage), la FSM reste fonctionnelle grâce à des temporisateurs de secours
 *    fondés sur les constantes de EMBCombatDefaults.
 */
UCLASS(ClassGroup = (Emberfall), meta = (BlueprintSpawnableComponent))
class EMBERFALL_API UEMBCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UEMBCombatComponent();

	// ---------------------------------------------------------------- Contrat (section 3.4)

	/** Serveur : esquive / parade / garde / dégâts. Retourne le résultat du coup. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	EEMBHitResult ReceiveHit(const FEMBHitData& Hit);

	/**
	 * Routage d'un coup vers une cible (serveur) :
	 * 1) la cible implémente IEMBHitReceiver -> ReceiveEMBHit ;
	 * 2) sinon UEMBCombatComponent -> ReceiveHit ;
	 * 3) sinon UEMBStatsComponent -> ApplyDamage + ApplyPoiseDamage ;
	 * 4) sinon Ignored.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	static EEMBHitResult ApplyHitToActor(AActor* Target, const FEMBHitData& Hit);

	/** Appelé par le personnage (Enhanced Input), sur le client propriétaire ou sur le serveur. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	void HandleInput(EEMBCombatInput Input);

	UFUNCTION(BlueprintPure, Category = "Emberfall|Combat")
	EEMBCombatState GetCurrentState() const { return CurrentState; }

	UFUNCTION(BlueprintPure, Category = "Emberfall|Combat")
	bool IsInvulnerable() const;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Combat")
	FEMBOnCombatStateChanged OnCombatStateChanged;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Combat")
	FEMBOnParrySuccess OnParrySuccess;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Combat")
	FEMBOnHitReceived OnHitReceived;

	/** Sur le client de l'attaquant : un de nos coups vient d'être résolu (hitmarker, son, hit-stop). Non fiable. */
	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Combat")
	FEMBOnHitConfirmed OnHitConfirmed;

	// ---------------------------------------------------------------- Lecture de l'état

	/** Vrai si la FSM autorise le passage vers NewState depuis l'état courant (table de transitions). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Combat")
	bool CanTransitionTo(EEMBCombatState NewState) const;

	/** Tags d'état actifs : State.Combat.Invulnerable, State.Combat.HyperArmor, State.Combat.Staggered, State.Dead. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Combat")
	bool HasStateTag(FGameplayTag Tag) const;

	/** Serveur : vrai pendant la fenêtre de riposte (1,5 s) qui suit une parade parfaite. */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Combat")
	bool IsRiposteWindowOpen() const;

	// ---------------------------------------------------------------- Configuration à l'exécution

	/** Équipement : mesh de l'arme dont les sockets servent à la détection. Par défaut, le mesh du personnage. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	void SetWeaponMesh(UMeshComponent* NewWeaponMesh);

	/** Dégâts de base de l'arme équipée (80 pour l'épée longue de référence). */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	void SetWeaponBaseDamage(float NewDamage);

	/**
	 * Bonus de passifs du Skill Tree (dégâts physiques, coût d'endurance des attaques, riposte, i-frames,
	 * coût de garde, fenêtre de parade). Poussé par UEMBProgressionComponent ; neutre (tout à 0) = valeurs de base.
	 */
	void SetPassiveModifiers(const FEMBPassiveModifiers& NewModifiers);

	/** Charge portée en proportion (0 = vide, 1 = limite, > 1 = surchargé : pas de roulade). Fournie par l'inventaire. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Combat")
	void SetEncumbranceRatio(float NewRatio);

	// ---------------------------------------------------------------- API appelée par les AnimNotifyState

	/** Composant de combat du propriétaire d'un mesh ; nullptr (sans erreur) dans l'éditeur d'animation. */
	static UEMBCombatComponent* FindFromMesh(const USkeletalMeshComponent* MeshComp);

	/** ComboWindow : ouvre/ferme la fenêtre de combo. À l'ouverture, un input mémorisé est consommé. */
	void SetComboWindowOpen(bool bOpen);

	/** IFrames : ajoute/retire un tag d'état (Invulnerable par défaut, HyperArmor accepté). */
	void BeginStateTag(const FGameplayTag& Tag);
	void EndStateTag(const FGameplayTag& Tag);

	/** HitWindow : début, tick (balayage de l'arme) et fin de la fenêtre de coup. Serveur uniquement. */
	void BeginHitWindow(const FEMBHitWindowParams& Params);
	void TickHitWindow(float DeltaTime);
	void EndHitWindow();

	// ---------------------------------------------------------------- Montages (EditDefaultsOnly, un par action)

	/** Combo léger : un montage par coup (3 entrées par défaut ; multiplicateur du 3e coup = x1,2). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Attaques")
	TArray<FEMBAttackDefinition> LightComboAttacks;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Attaques")
	FEMBAttackDefinition HeavyAttack;

	/** Frappe de l'attaque chargée. DamageMultiplier/PoiseDamage servent de minimum ; le maximum est dans EMBCombatDefaults. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Attaques")
	FEMBAttackDefinition ChargedAttack;

	/** Montage en boucle pendant la charge (maintien du bouton). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> ChargeLoopMontage;

	/** Roulades selon la charge portée (i-frames 13/11/9 frames pilotées par le notify IFrames du montage). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> DodgeMontageLight;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> DodgeMontageMedium;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> DodgeMontageHeavy;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> ParryMontage;

	/** Montage de garde : doit boucler (section en boucle) tant que la garde est maintenue. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> BlockMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> StaggerMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> GuardBreakMontage;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Montages")
	TObjectPtr<UAnimMontage> DeathMontage;

	// ---------------------------------------------------------------- Réglages d'équilibrage et de détection

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Equilibrage", meta = (ClampMin = "0"))
	float WeaponBaseDamage;

	/** Demi-angle (degrés) du cône frontal dans lequel garde et parade fonctionnent. Hypothèse de conception, hors référentiel. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Equilibrage", meta = (ClampMin = "1", ClampMax = "180"))
	float GuardHalfAngleDegrees;

	/**
	 * Tolérance de latence (s) ajoutée à la fin de la fenêtre de parade parfaite (10 frames = 0,167 s).
	 * La fenêtre étant mesurée en temps serveur, un joueur à 100 ms de ping la perçoit raccourcie ;
	 * 0 par défaut (valeur du référentiel), à ajuster en playtest réseau.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Equilibrage", meta = (ClampMin = "0", ClampMax = "0.2"))
	float ParryLatencyTolerance;

	/** Durée pendant laquelle un input est mémorisé (0,2 s = 12 frames). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Equilibrage", meta = (ClampMin = "0"))
	float InputBufferTime;

	/** Anti-abus : nombre maximal d'inputs acceptés par seconde et par joueur côté serveur. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Reseau", meta = (ClampMin = "1"))
	int32 MaxServerInputsPerSecond;

	/** Sockets de la lame (base et pointe) sur le mesh d'arme (ou du personnage). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection")
	FName WeaponTraceStartSocket;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection")
	FName WeaponTraceEndSocket;

	/**
	 * Canal de collision du balayage : EMBMeleeHit = ECC_GameTraceChannel1 (voir Config/DefaultEngine.ini).
	 * Configuration attendue : réponse par défaut Ignore ; Physics Assets des créatures et pawns en Overlap
	 * (le multi-sweep les rapporte tous, sans s'arrêter au premier, et les os touchés renseignent
	 * HitBoneName pour les points faibles) ; décor en Block (un mur protège ce qui est derrière) ;
	 * capsules de collision en Ignore (sinon on frappe la capsule et non le corps).
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection")
	TEnumAsByte<ECollisionChannel> HitTraceChannel;

	/** Nombre maximal de sous-pas d'interpolation par tick d'animation. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection", meta = (ClampMin = "1", ClampMax = "16"))
	int32 MaxHitSubSteps;

	/** Un sous-pas couvre au plus (rayon x ce facteur) de déplacement de la lame, pour ne pas "tunneler" une cible fine. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection", meta = (ClampMin = "0.1"))
	float SubStepTravelFactor;

	/** Autorise les coups entre joueurs (désactivé en coop). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Detection")
	bool bAllowFriendlyFire;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	// ---------------------------------------------------------------- RPC

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_HandleInput(EEMBCombatInput Input);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_PlayMontage(UAnimMontage* Montage, float PlayRate, FName StartSection);

	/** Variante fiable pour les montages dont la perte est visible : mort, étourdissement, garde, charge. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_PlayMontageReliable(UAnimMontage* Montage, float PlayRate, FName StartSection);

	/**
	 * Reliable : une garde en boucle dont l'arrêt serait perdu resterait figée chez les autres clients.
	 * Ciblé sur un montage précis : n'interrompt jamais une action lancée juste après.
	 */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_StopMontage(UAnimMontage* Montage, float BlendOutTime);

	/** Retour de coup vers le client propriétaire de l'attaquant. Non fiable : purement cosmétique. */
	UFUNCTION(Client, Unreliable)
	void Client_ConfirmHit(AActor* Target, EEMBHitResult Result, FVector Location);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_BroadcastHitReceived(const FEMBHitData& Hit, EEMBHitResult Result);

	UFUNCTION(NetMulticast, Unreliable)
	void Multicast_ParrySuccess(AActor* Attacker);

	UFUNCTION(Client, Reliable)
	void Client_RejectPrediction();

	UFUNCTION()
	void OnRep_CurrentState(EEMBCombatState OldState);

	UFUNCTION()
	void HandleOwnerDeath(AActor* Killer);

	UFUNCTION()
	void HandleOwnerPoiseBroken(AActor* InInstigator);

	// ---------------------------------------------------------------- FSM (serveur)

	void ProcessInput(EEMBCombatInput Input);
	bool ExecuteInput(EEMBCombatInput Input);
	bool TryStartLightAttack();
	bool TryStartHeavyAttack();
	bool TryStartCharge();
	bool TryStartDodge();
	bool TryStartParry();
	bool TryStartBlock();
	void ReleaseCharge();

	/** Vérifie CanTransitionTo, puis ExitState / changement / EnterState / diffusion. Serveur uniquement. */
	bool TransitionTo(EEMBCombatState NewState);
	void ExitState(EEMBCombatState OldState, EEMBCombatState NewState);
	void EnterState(EEMBCombatState NewState, EEMBCombatState OldState);
	void EnterStaggered(float Duration, bool bGuardBreak);

	/** Mémorise l'attaque active, lance son montage (ou la chronologie de secours) : commun aux 3 attaques. */
	void BeginAttackAction(const FEMBAttackDefinition& Definition, EEMBCombatState State);

	/** Fin naturelle de l'action courante (montage terminé ou temporisateur de secours). */
	void OnActionFinished();

	void ResetTransientFlags(EEMBCombatState NewState);

	// Buffer d'input
	bool IsBufferable(EEMBCombatInput Input) const;
	void BufferInput(EEMBCombatInput Input);
	void TryConsumeBufferedInput();
	bool AllowServerInput();

	// Résolution des coups
	EEMBHitResult ResolveHit(const FEMBHitData& Hit);
	EEMBHitResult ResolvePerfectParry(const FEMBHitData& Hit);
	EEMBHitResult ResolveBlockedHit(const FEMBHitData& Hit);
	EEMBHitResult ResolveUnguardedHit(const FEMBHitData& Hit);
	bool IsFacingAttacker(const FEMBHitData& Hit) const;
	bool IsVulnerableToCounterHit() const;

	// Détection des coups
	void SweepBlade(const FVector& Base, const FVector& Tip, const FVector& MotionDirection);
	FEMBHitData BuildHitData(const FHitResult& Impact, const FVector& MotionDirection) const;
	bool GetWeaponSocketLocations(FVector& OutBase, FVector& OutTip) const;
	USceneComponent* GetWeaponComponent() const;
	bool IsFriendlyTarget(const AActor* Target) const;

	// Montages
	UAnimInstance* GetOwnerAnimInstance() const;
	bool PlayActionMontage(UAnimMontage* Montage, float PlayRate, bool bTrackEnd, bool bReliable = false);
	void HandleMulticastPlayMontage(UAnimMontage* Montage, float PlayRate, FName StartSection);
	float GetDodgeIFrameEnd() const;
	float ScaledAttackCost(float BaseCost) const;
	static float GetBlockReduction(EEMBDamageType DamageType);
	void ApplyParryPosture(AActor* Attacker);
	void ArmBufferExpiry();
	bool PlayMontageLocal(UAnimMontage* Montage, float PlayRate, FName StartSection, bool bTrackEnd);
	UAnimMontage* GetDodgeMontage() const;
	void PredictLocally(EEMBCombatInput Input);

	// Temporisateurs d'action
	void ScheduleActionTimer(float Delay, TFunction<void()> Callback);
	void ClearActionTimers();
	void ScheduleFallbackTimeline(EEMBCombatState State);
	void ScheduleFallbackHitWindow(float Start, float End);
	void ScheduleStateTagWindow(float Start, float End, bool bHyperArmor);
	void ScheduleComboWindowFallback(float Open, float Close);

	// Divers
	bool HasAuthorityOwner() const;
	static bool IsAttackState(EEMBCombatState State);
	void ResolveDefaultTags();
	EEMBDamageType GetWeaponDamageType() const { return EEMBDamageType::Physical; }

	// ---------------------------------------------------------------- Données

	UPROPERTY(Transient)
	TObjectPtr<UEMBStatsComponent> Stats;

	UPROPERTY(Transient)
	TWeakObjectPtr<UMeshComponent> WeaponMesh;

	/** État courant, répliqué à tous. RepNotify : rediffuse OnCombatStateChanged côté client. */
	UPROPERTY(ReplicatedUsing = OnRep_CurrentState)
	EEMBCombatState CurrentState;

	/** Indice du coup de combo en cours (0..2). Répliqué : la prédiction client en déduit le coup suivant. */
	UPROPERTY(Replicated)
	uint8 ComboIndex;

	// Attaque en cours (serveur)
	FEMBAttackDefinition ActiveAttack;
	FEMBHitWindowParams ActiveHitParams;
	EEMBCombatState LastAttackState;

	// Drapeaux de fenêtre (serveur ; sur le client, alimentés localement par les notifies pour la prédiction)
	bool bComboWindowOpen;
	bool bDodgeCancelOpen;
	bool bParryWindowOpen;
	bool bParryWhiffed;
	/** Une parade parfaite a réussi dans cet état Parry : la fin de fenêtre ne doit plus la déclarer "ratée". */
	bool bParryLanded;
	bool bCharging;
	bool bHitWindowStarted;
	bool bHitWindowActive;
	int32 InvulnerableCount;
	int32 HyperArmorCount;

	// Entrées maintenues
	bool bBlockHeld;
	bool bChargeHeld;

	// Temps (monde serveur)
	double ChargeStartTime;
	double BlockGuardUpTime;
	double LastParryTime;
	double BufferedInputTime;
	double InputRateWindowStart;
	int32 InputRateCount;

	EEMBCombatInput BufferedInput;

	float EncumbranceRatio;

	/** Bonus de passifs courants (serveur) ; tout à 0 = neutre. */
	FEMBPassiveModifiers PassiveModifiers;
	float PendingStaggerDuration;
	bool bPendingGuardBreak;

	/** Jeton incrémenté à chaque nouvelle action : invalide les temporisateurs et callbacks de montage périmés. */
	uint32 ActionToken;

	/** Montage dont la fin pilote la FSM (serveur). */
	TWeakObjectPtr<UAnimMontage> ActiveMontage;
	bool bServerMontageStarted;

	/** Montage joué en prédiction sur le client propriétaire. */
	TWeakObjectPtr<UAnimMontage> PredictedMontage;

	TArray<FTimerHandle> ActionTimers;

	// Détection
	TSet<TWeakObjectPtr<AActor>> HitActorsThisWindow;

	/** Cible de la riposte critique (acteur paré) tant que la fenêtre de 1,5 s est ouverte. */
	TWeakObjectPtr<AActor> RiposteTarget;

	/** Expiration du buffer d'input (annule la prédiction client restée sans suite). */
	FTimerHandle BufferExpiryTimer;

	/** Tableaux réutilisés à chaque balayage pour éviter les allocations en plein combat. */
	TArray<FHitResult> ScratchImpacts;
	TArray<AActor*> ScratchAttached;
	TMap<AActor*, int32> ScratchBestImpact;
	FVector PrevWeaponBase;
	FVector PrevWeaponTip;
	bool bHasPrevWeaponPose;
};
