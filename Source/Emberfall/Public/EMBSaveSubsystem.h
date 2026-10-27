// Emberfall - sauvegarde du monde, des joueurs et des constructions (Dev B, référentiel section 8).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Engine/World.h"
#include "GameplayTagContainer.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "EMBSaveTypes.h"
#include "EMBSaveSubsystem.generated.h"

class APlayerController;
class APlayerState;
class AEMBBuildablePiece;
class UEMBSaveGame;
class USaveGame;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnSaveCompleted, bool, bSuccess);

/** bSuccess = vrai si une sauvegarde valide a été chargée ; faux = monde vierge (aucun slot valide). Dans les deux cas le chargement est terminé. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnLoadCompleted, bool, bSuccess);

/**
 * Sauvegarde d'Emberfall. Sous-système de GameInstance : il survit aux changements de niveau, et ne travaille
 * que sur le SERVEUR (hôte ou dédié).
 *
 * Intégrité des données :
 *  - Chaque monde (0..2) possède deux fichiers, EMB_World_<n>_A et EMB_World_<n>_B, écrits EN ALTERNANCE. Une écriture
 *    interrompue (crash, coupure) ne peut donc abîmer que le fichier en cours : l'autre reste intact.
 *  - Au chargement, les deux fichiers sont lus ; le plus récent qui est valide (lisible, version migrable, complet) est
 *    retenu. « Complet » = champs de contrôle de fin de fichier présents et cohérents (voir UEMBSaveGame).
 *  - Une seule écriture asynchrone est en vol ; une demande arrivée pendant ce temps est mise en attente (et regroupée).
 *  - Aucune écriture n'est possible tant que le chargement n'est pas terminé, ni si un fichier de version inconnue
 *    (écrit par un binaire plus récent) existe : on ne détruit jamais ce que l'on ne sait pas lire.
 *
 * Cycle de vie attendu du GameMode (serveur) : InitGame -> BindToWorld(World) puis SetActiveSlot puis LoadActiveSlot ;
 * PostLogin -> RestorePlayer ; Logout -> CapturePlayer ; EndPlay -> SaveNowBlocking.
 */
UCLASS(Config = Game)
class EMBERFALL_API UEMBSaveSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	// ----- Signatures figées (référentiel 8 et ajouts) -----

	/**
	 * Lie le sous-système au monde de jeu courant (synchrone). Appelé dans InitGame, AVANT SetActiveSlot. Remet l'état à
	 * zéro (chargement, caches, identités) et recrée les minuteries sur ce monde. NE FAIT RIEN si ce monde est déjà le
	 * monde lié (un second appel ne doit jamais effacer un chargement en cours ou terminé).
	 */
	void BindToWorld(UWorld* World);

	/** Monde actif, 0..2 (borné). Ignoré, avec avertissement, si un chargement est en cours ou terminé pour un autre monde. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Save")
	void SetActiveSlot(int32 SlotIndex);

	/**
	 * Serveur. Lit les deux fichiers du monde actif en asynchrone, retient le plus récent valide, restaure les événements
	 * monde et reconstruit les constructions. Monde vierge si aucun slot n'est valide. Termine par OnLoadCompleted.
	 * Idempotent : sans effet si le chargement est déjà en cours ou terminé pour le monde lié.
	 */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Save")
	void LoadActiveSlot();

	/**
	 * Serveur. Restaure la progression, l'inventaire, les PV/l'endurance et la position d'un joueur. Si le chargement du
	 * monde n'est pas terminé, la demande est mise en attente et traitée à la fin du chargement. Si le pawn n'existe pas
	 * encore, la restauration du pawn est retentée toutes les 0,25 s pendant PawnRestoreTimeoutSeconds, puis abandonnée.
	 * Si l'identifiant du joueur est déjà porté par un autre joueur connecté, ce joueur démarre un personnage neuf qui
	 * n'est PAS persisté (sinon les deux écraseraient mutuellement leurs données).
	 */
	void RestorePlayer(APlayerController* PC);

	/** Serveur. À appeler à la déconnexion d'un joueur (avant la destruction de son pawn) : enregistre son état, libère son identifiant, demande une sauvegarde. */
	void CapturePlayer(APlayerController* PC);

	/** Serveur. Écrit la sauvegarde. Sans bForce, elle est reportée tant qu'une rencontre de boss est active. */
	UFUNCTION(BlueprintCallable, Category = "Emberfall|Save")
	void RequestSave(bool bForce = false);

	/**
	 * Serveur. Écriture SYNCHRONE (SaveGameToSlot) vers le prochain fichier A/B, pour l'arrêt de l'hôte ou du serveur
	 * (EndPlay du GameMode), où une écriture asynchrone ne serait pas terminée. Ignorée si le chargement n'est pas
	 * terminé, si les écritures sont bloquées, ou si une écriture asynchrone est déjà en vol vers ce même fichier.
	 */
	void SaveNowBlocking();

	/**
	 * Identifiant stable d'un joueur (référentiel 8) :
	 *  1. APlayerState::GetUniqueId().ToString() si l'identifiant vient d'un vrai service (Steam, EOS...) ;
	 *  2. sinon, l'hôte local reçoit la clé fixe "LocalHost" ;
	 *  3. sinon, "Guest_<nom du joueur assaini>" (NON garanti unique : deux invités de même nom sont détectés au
	 *     RestorePlayer, le second démarre un personnage neuf non persisté).
	 * Limite : avec l'Online Subsystem NULL (PIE, LAN), l'identifiant d'un client change d'une session à l'autre ;
	 * il est alors traité comme un invité identifié par son nom.
	 */
	static FString GetStablePlayerId(const APlayerState* PS);

	/**
	 * Table DT_BuildPieces configurée (DefaultGame.ini). Source unique : le composant de construction l'utilise quand sa
	 * propre table est vide, de sorte que la construction et la sauvegarde lisent forcément la même.
	 */
	static TSoftObjectPtr<UDataTable> GetConfiguredBuildPiecesTable();

	/**
	 * Clé d'un joueur pour les sacs et les liaisons : son identifiant stable réclamé à RestorePlayer ; pour un joueur
	 * non persisté (identifiant en doublon), "Session_<PlayerId>" (jamais sauvegardé). Vide sans PlayerState.
	 */
	FString GetPlayerKey(const APlayerController* PC) const;

	/** Faux si le joueur a été refusé comme doublon d'identifiant (ses données ne sont pas sauvegardées). */
	bool IsPlayerPersisted(const APlayerController* PC) const;

	/**
	 * Dernier point sûr relevé pour cet identifiant (position vivante, au sol, hors combat, relevée toutes les 10 s).
	 * Exposé pour le sac de mort quand la mort a lieu hors sol (chute, vide).
	 */
	bool GetLastSafeTransform(const FString& StableId, FTransform& OutTransform) const;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Save")
	FEMBOnSaveCompleted OnSaveCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Emberfall|Save")
	FEMBOnLoadCompleted OnLoadCompleted;

	// ----- Compléments -----

	UFUNCTION(BlueprintPure, Category = "Emberfall|Save")
	int32 GetActiveSlot() const { return ActiveSlot; }

	/** Vrai quand le chargement du monde actif est terminé (sauvegarde restaurée ou monde vierge). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Save")
	bool IsLoaded() const { return LoadState == ELoadState::Loaded; }

	/** Vrai si les écritures sont bloquées pour ne pas détruire un fichier illisible (version inconnue, table absente...). */
	UFUNCTION(BlueprintPure, Category = "Emberfall|Save")
	bool IsWriteBlocked() const { return bBlockedByUnusableSave; }

	/** Intervalle de l'autosave (référentiel : 300 s). Réglable dans DefaultGame.ini. */
	UPROPERTY(Config)
	float AutosaveIntervalSeconds = 300.f;

	/** Intervalle de relevé du dernier point sûr des joueurs (référentiel : 10 s). */
	UPROPERTY(Config)
	float SafePointIntervalSeconds = 10.f;

	/** Durée maximale pendant laquelle la restauration du pawn d'un joueur est retentée. */
	UPROPERTY(Config)
	float PawnRestoreTimeoutSeconds = 20.f;

	/**
	 * DT_BuildPieces : sert à valider les pièces sauvegardées (ligne existante) et à en reprendre classe, matériau et
	 * catégorie. DOIT être la même table que celle du composant de construction. Réglable dans DefaultGame.ini.
	 * Absente alors que la sauvegarde contient des constructions : écritures bloquées (on ne les détruit pas).
	 */
	UPROPERTY(Config)
	TSoftObjectPtr<UDataTable> BuildPiecesTable;

private:
	enum class ELoadState : uint8
	{
		NotLoaded,
		Loading,
		Loaded
	};

	/** Restauration de pawn à retenter (le pawn n'existait pas encore). */
	struct FPawnRestoreRetry
	{
		TWeakObjectPtr<APlayerController> Controller;
		double DeadlineTime = 0.0;
	};

	// ----- Monde courant et minuteries -----
	UWorld* GetServerWorld() const;
	bool IsServer() const;
	void StartWorldTimers(UWorld* World);
	void HandleAutosaveTimer();
	void HandleSafePointTimer();
	void HandleRestoreRetryTimer();

	UFUNCTION()
	void HandleWorldEventRecorded(FGameplayTag EventTag);

	// ----- Chargement -----
	void OnSlotLoaded(uint32 Generation, const FString& SlotName, USaveGame* LoadedData);
	void FinishLoading();
	void ApplyLoadedSave(UWorld* World, UEMBSaveGame* Save);
	void SpawnSavedPieces(UWorld* World, const TArray<FEMBPieceSaveData>& SavedPieces);

	// ----- Identités et joueurs -----
	bool ClaimIdentity(APlayerController* PC);
	void ReleaseIdentity(APlayerController* PC);
	FString GetControllerId(const APlayerController* PC) const;
	bool IsRestorePending(const APlayerController* PC) const;
	void RestoreFromCache(APlayerController* PC);
	void BuildPlayerData(APlayerController* PC, FEMBPlayerSaveData& OutData) const;
	void CaptureAllPlayers();
	void ApplyProgression(APlayerController* PC, const FEMBPlayerSaveData& Data) const;
	bool TryRestorePawn(APlayerController* PC, const FEMBPlayerSaveData& Data) const;
	void ProcessPendingRestores();

	// ----- Écriture -----
	UEMBSaveGame* BuildSnapshot();
	void StartWrite();
	void OnSlotSaved(const FString& SlotName, const int32 UserIndex, bool bSuccess);
	/** Capture les pièces posées par des joueurs ; renvoie aussi le SaveId attribué à chaque pièce (liaisons de réapparition). */
	void CapturePieces(TArray<FEMBPieceSaveData>& OutPieces, TMap<const AEMBBuildablePiece*, int32>& OutSaveIds) const;
	void CaptureDeathBags(TArray<FEMBDeathBagSaveData>& OutBags) const;
	FString BuildSlotName(bool bSlotA) const;
	bool IsBossEncounterActive() const;

private:
	int32 ActiveSlot = 0;
	ELoadState LoadState = ELoadState::NotLoaded;

	/** Monde auquel le sous-système est lié (BindToWorld). */
	TWeakObjectPtr<UWorld> BoundWorld;

	/** Monde pour lequel le chargement en cours a été lancé, et jeton de génération : invalident les callbacks asynchrones périmés. */
	TWeakObjectPtr<UWorld> LoadWorld;
	uint32 LoadGeneration = 0;

	/** Fichiers lus pendant le chargement. */
	UPROPERTY(Transient)
	TObjectPtr<UEMBSaveGame> LoadedSlotA;

	UPROPERTY(Transient)
	TObjectPtr<UEMBSaveGame> LoadedSlotB;

	int32 PendingLoadCount = 0;

	/** Prochain fichier à écrire : l'opposé du dernier écrit ou chargé. */
	bool bNextWriteToSlotA = true;
	bool bCurrentWriteToSlotA = true;

	bool bWriteInFlight = false;

	/** LoadGeneration au moment où l'écriture en vol a démarré : un callback d'un monde précédent ne touche pas l'état du monde courant. */
	uint32 InFlightWriteGeneration = 0;
	bool bSavePending = false;
	bool bSaveDeferredByBoss = false;

	/** Instant (secondes du monde) où la sauvegarde a commencé à être reportée à cause d'un boss. */
	double DeferredSinceTime = 0.0;

	/** Bloc météo du fichier chargé, réécrit si UEMBWeatherSubsystem::ExportSaveData échoue (état pas prêt). */
	FEMBWeatherSaveData RetainedWeather;
	bool bHasRetainedWeather = false;

	/** Un fichier illisible (version inconnue...) existe : aucune écriture, sous peine de le détruire. */
	bool bBlockedByUnusableSave = false;

	/** Données de tous les joueurs connus (connectés ou non), par identifiant stable. */
	TMap<FString, FEMBPlayerSaveData> PlayerCache;

	/** Dernier point sûr relevé, par identifiant stable. */
	TMap<FString, FTransform> LastSafeTransforms;

	/** Identifiant stable porté par chaque contrôleur actif (calculé une fois à RestorePlayer, réutilisé partout). */
	TMap<TWeakObjectPtr<APlayerController>, FString> ControllerIds;

	/** Identifiants actuellement portés par un contrôleur vivant (détection des doublons). */
	TMap<FString, TWeakObjectPtr<APlayerController>> ActiveIds;

	/** Contrôleurs dont l'identifiant est déjà pris : personnage neuf, jamais persisté. */
	TSet<TWeakObjectPtr<APlayerController>> UnpersistedControllers;

	/** Joueurs arrivés avant la fin du chargement du monde. */
	TArray<TWeakObjectPtr<APlayerController>> PendingRestoreControllers;

	TArray<FPawnRestoreRetry> PawnRestoreRetries;

	FTimerHandle AutosaveTimerHandle;
	FTimerHandle SafePointTimerHandle;
	FTimerHandle RestoreRetryTimerHandle;
};
