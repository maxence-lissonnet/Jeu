# Emberfall — Architecture du module C++

> **Statut :** v1.2 (23/10/2026), mise à jour après le codage de la **progression** et de la **sauvegarde**, à partir d'une relecture du code du module `Emberfall` (`Source/Emberfall/Public` et `Private` : 35 en-têtes, 29 `.cpp`, environ 16 900 lignes) et du référentiel v1.2 (§ 7 et 8). **Le code n'a jamais été compilé** : ce document décrit ce qui est écrit, pas un comportement mesuré.
> **Changements v1.1 → v1.2 :** classes `AEMBGameMode`, `AEMBPlayerState`, `UEMBProgressionComponent`, `UEMBProgressionSubsystem`, `UEMBProgressionSettings`, `AEMBDiscoveryVolume`, `UEMBSaveSubsystem`, `UEMBSaveGame` et types `EMBSaveTypes.h` ; ajouts dans le combat (`SetPassiveModifiers`), les jauges (`RestoreFromSave`), la construction (multiplicateur de coût, colonne `RequiredGlyph`, `IsPieceUnlocked` branché), l'inventaire (bonus de charge, export/import), la structure (`BeginBulkLoad` / `EndBulkLoad`), le dragon (`NotifyWorldEvent`, `IsEncounterActive`, `ApplyDefeatedFromSave`, `OnEncounterEnded`) et l'arène (état final calme). Données : `Data/DT_SkillNodes.csv`, colonne `RequiredGlyph` de `DT_BuildPieces.csv`, réglages dans `Config/DefaultGame.ini`. Décision utilisateur : **un acteur répliqué par pièce pour la v1** (le Fast Array reste une évolution). Sections réécrites : 0, 1, 2.1, 2.7 à 2.10 (nouvelles), 3, 5, 6, 7, 8, 9, 10.
> **Changements v1 → v1.1 :** mouvement dédié `UEMBCharacterMovementComponent` (sprint par « saved moves »), retour de coup `Client_ConfirmHit` / `OnHitConfirmed`, montages fiables `Multicast_PlayMontageReliable`, riposte ×3, i-frames par timer, parade routée par `IEMBHitReceiver`, pièces de construction qui reçoivent les coups, `PlacerPlayerId` et `GetBaseId`, recalcul limité au sous-graphe dépendant et cascade par niveaux, inventaire pondéré, foudre de phase 3, VFX des zones de feu, fichiers projet et CSV de données. Le détail de chaque écart corrigé ou encore ouvert est en section 9.
> **Règle de lecture :** tout ce qui n'existe pas dans le code est marqué **« prévu, non codé »**. Les noms de classes, de membres, de délégués et de RPC cités sans cette mention existent tels quels dans les en-têtes.
> **Références :** constantes et contrats dans `00_Constantes_et_Contrats.md` (le « référentiel ») ; budgets et décisions de performance dans `03_Scalabilite.md`.
> **Moteur :** Unreal Engine 5.4+, coop 1 à 4 joueurs, serveur autoritaire (listen ou dédié).

---

## 0. Synthèse

| # | Constat ou décision | Statut |
|---|---|---|
| A1 | Le jeu repose sur **des composants d'acteur** (`UEMBStatsComponent`, `UEMBCombatComponent`, `UEMBInventoryComponent`, `UEMBBuildingComponent`) portés par `AEMBPlayerCharacter`, et non sur le Gameplay Ability System. | Codé |
| A2 | Un seul point d'entrée pour tous les dégâts : `UEMBCombatComponent::ApplyHitToActor` (routage interface → FSM → jauges). | Codé |
| A3 | Le dragon est un `ACharacter` serveur-autoritaire piloté par un Behavior Tree (3 tâches, 1 service, 1 décorateur C++) ; l'arène se met à jour **sans réplication** à partir de la phase répliquée du boss. | Codé (les assets BT/BB sont à créer) |
| A4 | La construction utilise **un acteur répliqué dormant par pièce** (`AEMBBuildablePiece`, qui reçoit aussi les coups via `IEMBHitReceiver`) et un graphe d'intégrité dans un `UWorldSubsystem` (recalcul limité au sous-graphe dépendant, destructions regroupées par frame, cascade par niveaux, chargement en masse). **Décision utilisateur : acteur par pièce pour la v1** ; l'acteur « Base » à **Fast Array** (`03_Scalabilite.md` D5, Q4) reste une évolution. | Codé (acteur par pièce, retenu v1) / Fast Array : évolution |
| A5 | Gameplay Framework : `AEMBGameMode` (branche la sauvegarde sur le cycle de vie de la partie, déclare `AEMBPlayerState` et `AEMBPlayerCharacter`) et `AEMBPlayerState` (porte la progression) sont codés. **GameState et PlayerController propres n'existent pas**, et **aucune réapparition (respawn)** n'est codée : un joueur mort reste en état `Dead`, entrées désactivées. | Partiel |
| A6 | Progression : `UEMBProgressionComponent` sur `AEMBPlayerState` (état répliqué), `UEMBProgressionSubsystem` (`UWorldSubsystem`, règles et événements monde), `UEMBProgressionSettings` (`UDeveloperSettings`, barèmes dans `DefaultGame.ini`), `AEMBDiscoveryVolume` (lieux découverts). XP, niveaux, points, glyphes, rangs de compétences et passifs de combat, construction et charge sont appliqués (section 5). Passifs de survie et compétences actives : **non appliqués**. | Codé (réserves section 9) |
| A7 | Sauvegarde : `UEMBSaveSubsystem` (`UGameInstanceSubsystem`, serveur uniquement), `UEMBSaveGame`, structures `EMBSaveTypes.h` ; 3 mondes × 2 fichiers A/B écrits en alternance, autosave 300 s / boss vaincu / déconnexion, report pendant une rencontre de boss, chargement en masse des constructions (section 6). | Codé (réserves section 9) |
| A8 | Interface utilisateur (UMG) : aucun widget n'est codé ; les délégués nécessaires au HUD, à l'arbre de glyphes et aux sauvegardes existent. | Prévu, non codé |

---

## 1. Vue d'ensemble : les couches

```
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│ UI (UMG)                    prévu, non codé — s'abonne aux délégués (jauges, état de combat,  │
│                             inventaire, résultat de placement, phase du boss)                 │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Gameplay Framework          AEMBGameMode (cycle de vie de la sauvegarde) · AEMBPlayerState    │
│                             (progression). GameState / PlayerController : classes moteur.     │
│                             Pawns codés : AEMBPlayerCharacter, AEMBDragonBoss (ACharacter)    │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Composants de jeu           UEMBStatsComponent (joueur ET boss) · UEMBCombatComponent (FSM)   │
│                             UEMBInventoryComponent (poids) · UEMBBuildingComponent            │
│                             UEMBCharacterMovementComponent (sprint prédit, saved moves)       │
│                             UEMBProgressionComponent (sur le PlayerState)                     │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Acteurs de monde            AEMBBuildablePiece (pièce posée, IEMBHitReceiver) ·               │
│                             AEMBDragonArena (décor par phase, lave, foudre, état final calme) │
│                             AEMBDiscoveryVolume (lieu découvert, serveur)                     │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Subsystems                  UEMBStructureSubsystem (UWorldSubsystem : graphe, intégrité,      │
│                             effondrement, chargement en masse) · UEMBProgressionSubsystem     │
│                             (UWorldSubsystem : tables, événements monde) · UEMBSaveSubsystem  │
│                             (UGameInstanceSubsystem : fichiers A/B, autosave, serveur seul)   │
│ Réglages                    UEMBProgressionSettings (UDeveloperSettings, DefaultGame.ini)     │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ IA (serveur uniquement)     AEMBDragonAIController (perception vue, Blackboard)               │
│                             BTTask_EMBSelectAttack · BTTask_EMBDragonBreath ·                 │
│                             BTTask_EMBDragonDiveAttack · BTService_EMBUpdateCombatContext ·   │
│                             BTDecorator_EMBDragonPhase                                        │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Animation                   EMBAnimNotifyState_HitWindow / _ComboWindow / _IFrames            │
├──────────────────────────────────────────────────────────────────────────────────────────────┤
│ Données                     CSV Data/ : DT_BuildPieces (FEMBBuildPieceRow, + RequiredGlyph),  │
│                             DT_DragonAttacks (FEMBDragonAttackRow), DT_Glyphs (FEMBGlyphRow), │
│                             DT_SkillNodes (FEMBSkillNodeRow, + PassiveStat / ValuePerRank)    │
│                             Sauvegarde : UEMBSaveGame + FEMBPlayer/Piece/WorldSaveData        │
│                             Constantes C++ : EMBCombatDefaults, EMBBuildingDefaults,          │
│                             EMBDragonDefaults · Gameplay Tags natifs : EMBTags                │
│                             Config : canaux EMBMeleeHit / EMBBuilding, tags Glyph.*           │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
```

| Couche | Ce qui existe | Ce qui manque |
|---|---|---|
| **GameMode** | `AEMBGameMode` (`AGameModeBase`) : déclare `AEMBPlayerState` et `AEMBPlayerCharacter` ; `InitGame` → `BindToWorld` + `SetActiveSlot` (option d'URL `?Slot=N`), `StartPlay` → `LoadActiveSlot`, `PostLogin` → `RestorePlayer`, `Logout` → `CapturePlayer`, `EndPlay` (arrêt ou fin de PIE) → `SaveNowBlocking` | **réapparition** des joueurs (un joueur mort n'est jamais relancé) ; réinitialisation du boss après un wipe |
| **GameState** | — (classe moteur) | état de monde **répliqué** : les événements monde (`WorldEvents`) vivent dans `UEMBProgressionSubsystem`, **serveur seul** ; horloge jour/nuit non codée |
| **PlayerController** | — (`APlayerController` moteur) | `AEMBPlayerController` : HUD, menus, ouverture de l'arbre de glyphes (prévu). Les entrées sont liées dans le **pawn** (`AEMBPlayerCharacter::SetupPlayerInputComponent`). |
| **PlayerState** | `AEMBPlayerState` : porte `UEMBProgressionComponent`, `NetUpdateFrequency` 10 Hz, `CopyProperties` / `OverrideWith` (voyage transparent, reconnexion d'un joueur inactif) par export/import de la progression | — |
| **Character** | `AEMBPlayerCharacter` (`PossessedBy` réapplique les passifs), `AEMBDragonBoss` | — |
| **Composants** | 4 composants de jeu + `UEMBCharacterMovementComponent` + `UEMBProgressionComponent` (voir § 2) | — |
| **Subsystems** | `UEMBStructureSubsystem`, `UEMBProgressionSubsystem` (monde), `UEMBSaveSubsystem` (GameInstance) | — |
| **IA** | contrôleur + 5 nœuds BT | assets `BT_Vaelthyrax` et `BB_Vaelthyrax` à créer dans l'éditeur |
| **Données** | structures de lignes + CSV `Data/DT_BuildPieces.csv` (31 lignes, colonne `RequiredGlyph`), `DT_DragonAttacks.csv` (11), `DT_Glyphs.csv` (36), `DT_SkillNodes.csv` (20) | import en DataTables dans l'éditeur (`/Game/Data/`, chemins attendus par `DefaultGame.ini`) |
| **Projet** | `Emberfall.uproject` (UE 5.4, plugins EnhancedInput et Niagara), `Emberfall.Target.cs`, `EmberfallEditor.Target.cs`, `EmberfallServer.Target.cs`, `Config/DefaultEngine.ini` (Enhanced Input, canaux de collision, `GlobalDefaultGameMode=/Script/Emberfall.EMBGameMode`), `DefaultGame.ini` (progression, sauvegarde, cuisson de `/Game/Data`), `Config/Tags/EmberfallGlyphs.ini` | réglages réseau (débits, Push Model), cartes par défaut |
| **UI (UMG)** | dépendance `UMG` déclarée dans `Emberfall.Build.cs` | aucun widget (prévu) |

**Principes visibles dans le code**
- **Aucun Tick de gameplay permanent.** Le personnage, le combat, le boss, les pièces, la progression et la sauvegarde n'ont pas de Tick (la sauvegarde utilise trois timers : autosave 300 s, relevé du point sûr 10 s, nouvelle tentative de restauration de pawn 0,25 s). Seuls tournent : la régénération de `UEMBStatsComponent` (Tick réveillé à la demande, 20 Hz, serveur), l'aperçu de `UEMBBuildingComponent` (30 Hz, client local), les tâches latentes du BT (`bNotifyTick`) et le **Tick du contrôleur IA du dragon**, volontairement réactivé : `AAIController::Tick` appelle `UpdateControlRotation`, qui applique le focus à la rotation du dragon (coût d'une rotation, serveur seul). Le reste passe par des RPC, des notifies, des timers (sprint, brûlure, zones de feu, lave, foudre, vagues d'effondrement) et des délégués.
- **Serveur seul juge.** Toute modification de jauge, d'inventaire, d'état de combat, de pièce ou de boss est gardée par `HasAuthority()`.
- **Constantes centralisées.** Les valeurs du référentiel sont recopiées dans `EMBCombatDefaults`, `EMBBuildingDefaults`, `EMBDragonDefaults` ; les composants les exposent en `UPROPERTY(EditDefaultsOnly)` quand elles doivent être réglables en Blueprint.

---

## 2. Diagrammes

### 2.1 Diagramme de classes principal

Légende : `*--` composition (sous-objet créé dans le constructeur) ; `<|--` héritage ; `<|..` implémentation d'interface ; `..>` dépendance (appel) ; `o--` agrégation (référence sans possession). `<<prévu>>` = non codé.

```mermaid
classDiagram
    direction TB

    class ACharacter
    class AAIController
    class UActorComponent
    class UWorldSubsystem
    class UAnimNotifyState
    class UBTTaskNode
    class UBTService
    class UBTDecorator_BlackboardBase

    class IEMBHitReceiver {
        <<interface>>
        +ReceiveEMBHit(FEMBHitData) EEMBHitResult
    }
    class IGameplayTagAssetInterface {
        <<interface>>
        +GetOwnedGameplayTags(FGameplayTagContainer)
    }

    class AEMBPlayerController {
        <<prévu>>
    }
    class AEMBPlayerState {
        +GetProgressionComponent() UEMBProgressionComponent
    }
    class UEMBProgressionComponent {
        +ApplyModifiersToPawn(APawn)
        +HasGlyph(FName) bool
    }
    class UEMBProgressionSubsystem {
        +NotifyWorldEvent(FGameplayTag, AActor, int64, float)
        +IsWorldEventRecorded(FGameplayTag) bool
    }

    class AEMBPlayerCharacter {
        -USpringArmComponent SpringArm
        -UCameraComponent Camera
        -FTimerHandle SprintDrainTimerHandle
        +GetStatsComponent() UEMBStatsComponent
        +GetCombatComponent() UEMBCombatComponent
        +IsSprinting() bool
        +NotifySprintRequested(bool)
        +PossessedBy(AController)
        -SendCombatInput(EEMBCombatInput)
        -HandleCombatStateChanged(EEMBCombatState, EEMBCombatState)
        -HandleInventoryChanged(FName, int32)
        -UpdateEncumbrance()
        -ForceStopSprint()
        -Client_StopSprint()
    }

    class UCharacterMovementComponent
    class UEMBCharacterMovementComponent {
        +float WalkSpeed
        +float SprintSpeed
        +uint8 bWantsToSprint
        -uint8 bSprintForbidden
        +SetWantsToSprint(bool)
        +IsSprintActive() bool
        +ForbidSprint()
        +GetMaxSpeed() float
        +UpdateFromCompressedFlags(uint8)
    }
    class FEMBSavedMove {
        +uint8 bSavedWantsToSprint
        +GetCompressedFlags() uint8
    }

    class UEMBStatsComponent {
        -float Health
        -float Stamina
        -float Poise
        -float RepPoise
        +float MaxHealth
        +ConsumeStamina(float) bool
        +ApplyDamage(float, AActor) float
        +ApplyPoiseDamage(float, AActor) bool
        +SetMaxHealth(float, bool)
        +RestoreFromSave(float, float)
        +OnAttributeChanged
        +OnDeath
        +OnPoiseBroken
    }

    class UEMBCombatComponent {
        -EEMBCombatState CurrentState
        -uint8 ComboIndex
        -EEMBCombatInput BufferedInput
        +HandleInput(EEMBCombatInput)
        +ReceiveHit(FEMBHitData) EEMBHitResult
        +ApplyHitToActor(AActor, FEMBHitData)$
        +CanTransitionTo(EEMBCombatState) bool
        +IsRiposteWindowOpen() bool
        +SetEncumbranceRatio(float)
        +SetPassiveModifiers(FEMBPassiveModifiers)
        +BeginHitWindow(FEMBHitWindowParams)
        +SetComboWindowOpen(bool)
        -Server_HandleInput(EEMBCombatInput)
        -Multicast_PlayMontage(UAnimMontage, float, FName)
        -Multicast_PlayMontageReliable(UAnimMontage, float, FName)
        -Multicast_StopMontage(UAnimMontage, float)
        -Client_ConfirmHit(AActor, EEMBHitResult, FVector)
        -Client_RejectPrediction()
        -ApplyParryPosture(AActor)
        +OnCombatStateChanged
        +OnParrySuccess
        +OnHitReceived
        +OnHitConfirmed
    }

    class UEMBInventoryComponent {
        -TArray~FEMBResourceEntry~ Resources
        +float MaxCarryWeight
        +GetResourceCount(FName) int32
        +HasResources(TMap) bool
        +ConsumeResources(TMap) bool
        +AddResource(FName, int32)
        +GetCurrentWeight() float
        +GetUnitWeight(FName)$
        -float CarryWeightBonus
        +SetCarryWeightBonus(float)
        +ExportResources(TArray)
        +ImportResources(TArray)
        +OnInventoryChanged
    }

    class UEMBBuildingComponent {
        -bool bBuildMode
        -FName SelectedRowName
        -FEMBPlacementEvaluation LastEvaluation
        +ToggleBuildMode()
        +SetSelectedPiece(FName)
        +ConfirmPlacement()
        -float BuildCostMultiplier
        +IsPieceUnlocked(FName) bool
        +SetBuildCostMultiplier(float)
        +GetEffectiveCost(Row) TMap
        -EvaluatePlacement(Row, CDO, FTransform, bool) FEMBPlacementEvaluation
        #Server_PlacePiece(FName, FTransform)
        #Client_PlacementResult(EEMBPlacementResult, float)
        +OnBuildModeChanged
        +OnPlacementResult
    }

    class AEMBBuildablePiece {
        #FName PieceRowName
        #EEMBBuildMaterial Material
        #float CurrentHealth
        #float Integrity
        #bool bCollapsed
        #int32 PlacerPlayerId
        -FString PlacerStableId
        +InitializePiece(FName, EEMBBuildMaterial, EEMBPieceCategory, bool, int32)
        +GetPlacerPlayerId() int32
        +GetPlacerStableId() FString
        +RestoreFromSave(float, FString)
        +ReceiveEMBHit(FEMBHitData) EEMBHitResult
        +ApplyStructuralDamage(float, AActor) float
        +TakeDamage(...) float
        +Collapse()
        +SetIntegrityServer(float)
        +ExecuteCollapse(bool)
        #Multicast_PlayCollapseEffects(bool)
    }

    class UEMBStructureSubsystem {
        -TArray~FNode~ Nodes
        -TMap Cells
        -TArray CollapseQueue
        -TArray DirectCollapseRequests
        +RegisterPiece(AEMBBuildablePiece)
        +UnregisterPiece(AEMBBuildablePiece, bool)
        +RequestCollapse(AEMBBuildablePiece)
        +CollapsePiecesNow(TArray)
        +PredictIntegrity(FEMBStructureProbe, int32) float
        +CountPiecesInTouchedBases(FEMBStructureProbe, int32) int32
        +GetBaseId(AEMBBuildablePiece) int32
        +BeginBulkLoad()
        +EndBulkLoad() int32
        +GetAllPieces(TArray)
        -CollectDependentNodes(TArray, TArray) uint32
        -RecomputeAffected(TArray, uint32, FVector)
    }

    class AEMBDragonBoss {
        -int32 CurrentPhase
        -bool bIsTransitioning
        -bool bIsStunned
        -bool bIsDead
        -bool bIsFlying
        -bool bIsVulnerable
        #UEMBStatsComponent StatsComponent
        +ReceiveEMBHit(FEMBHitData) EEMBHitResult
        +BeginTimedAttack(FEMBDragonAttackRow) float
        +ApplyAreaDamage(...) TArray
        +ApplyConeDamage(...) TArray
        +BeginEncounter()
        +AddFireZone(FVector, float, float, float)
        +Multicast_SpawnFireZoneVFX(UNiagaraSystem, TArray)
        +Multicast_ShowGroundMarker(FVector, float, float, bool)
        +IsEncounterActive() bool
        +ApplyDefeatedFromSave()
        +OnPhaseChanged
        +OnPhaseTransitionStarted
        +OnStunChanged
        +OnBossDefeated
        +OnEncounterEnded
    }

    class AEMBDragonAIController {
        +FName BBKey_TargetActor$
        +SyncBlackboardState()
        +SetCombatTarget(AActor)
        +SetFacingLocked(bool)
        #HandleTargetPerceptionUpdated(AActor, FAIStimulus)
    }

    class AEMBDragonArena {
        #TArray~FEMBArenaPhaseConfig~ PhaseConfigs
        #float LavaDamagePerSecond
        +BindToBoss(AEMBDragonBoss)
        #HandleBossPhaseChanged(int32, int32)
        #OnAshStormChanged(bool)
        -StartLightning()
        -StrikeLightning()
        -ResolveLightningImpact(FVector)
        -EnterFinalCalmState()
        #OnArenaFinalCalmState()
    }

    class BTTask_EMBSelectAttack
    class BTTask_EMBDragonBreath
    class BTTask_EMBDragonDiveAttack
    class BTService_EMBUpdateCombatContext
    class BTDecorator_EMBDragonPhase

    class EMBAnimNotifyState_HitWindow
    class EMBAnimNotifyState_ComboWindow
    class EMBAnimNotifyState_IFrames

    class FEMBGlyphRow {
        <<DataTable row>>
    }
    class FEMBSkillNodeRow {
        <<DataTable row>>
    }

    ACharacter <|-- AEMBPlayerCharacter
    ACharacter <|-- AEMBDragonBoss
    AAIController <|-- AEMBDragonAIController
    UActorComponent <|-- UEMBStatsComponent
    UActorComponent <|-- UEMBCombatComponent
    UActorComponent <|-- UEMBInventoryComponent
    UActorComponent <|-- UEMBBuildingComponent
    UWorldSubsystem <|-- UEMBStructureSubsystem
    UBTTaskNode <|-- BTTask_EMBSelectAttack
    UBTTaskNode <|-- BTTask_EMBDragonBreath
    UBTTaskNode <|-- BTTask_EMBDragonDiveAttack
    UBTService <|-- BTService_EMBUpdateCombatContext
    UBTDecorator_BlackboardBase <|-- BTDecorator_EMBDragonPhase
    UAnimNotifyState <|-- EMBAnimNotifyState_HitWindow
    UAnimNotifyState <|-- EMBAnimNotifyState_ComboWindow
    UAnimNotifyState <|-- EMBAnimNotifyState_IFrames
    UCharacterMovementComponent <|-- UEMBCharacterMovementComponent
    IEMBHitReceiver <|.. AEMBDragonBoss
    IEMBHitReceiver <|.. AEMBBuildablePiece
    IGameplayTagAssetInterface <|.. AEMBDragonBoss

    AEMBPlayerCharacter *-- UEMBStatsComponent
    AEMBPlayerCharacter *-- UEMBCombatComponent
    AEMBPlayerCharacter *-- UEMBInventoryComponent
    AEMBPlayerCharacter *-- UEMBBuildingComponent
    AEMBPlayerCharacter *-- UEMBCharacterMovementComponent : SetDefaultSubobjectClass
    UEMBCharacterMovementComponent ..> FEMBSavedMove : rejeu réseau
    UEMBCharacterMovementComponent ..> AEMBPlayerCharacter : NotifySprintRequested (serveur)
    AEMBDragonBoss *-- UEMBStatsComponent
    AEMBPlayerState *-- UEMBProgressionComponent
    AEMBPlayerController ..> AEMBPlayerCharacter : possède (prévu)
    UEMBProgressionComponent ..> UEMBCombatComponent : SetPassiveModifiers (serveur)
    UEMBProgressionComponent ..> UEMBBuildingComponent : SetBuildCostMultiplier (serveur)
    UEMBProgressionComponent ..> UEMBInventoryComponent : SetCarryWeightBonus (serveur)
    UEMBBuildingComponent ..> UEMBProgressionComponent : IsPieceUnlocked lit HasGlyph
    AEMBPlayerCharacter ..> UEMBProgressionComponent : PossessedBy, ApplyModifiersToPawn
    AEMBDragonArena ..> AEMBDragonBoss : OnEncounterEnded (état final calme)

    UEMBCombatComponent ..> UEMBStatsComponent : FindComponentByClass
    UEMBCombatComponent ..> IEMBHitReceiver : ApplyHitToActor, ApplyParryPosture
    AEMBPlayerCharacter ..> UEMBInventoryComponent : OnInventoryChanged
    AEMBPlayerCharacter ..> UEMBCombatComponent : SetEncumbranceRatio
    UEMBBuildingComponent ..> UEMBInventoryComponent : ConsumeResources
    UEMBBuildingComponent ..> AEMBBuildablePiece : SpawnActorDeferred
    UEMBBuildingComponent ..> UEMBStructureSubsystem : PredictIntegrity, CountPiecesInTouchedBases
    UEMBStructureSubsystem o-- AEMBBuildablePiece : graphe FNode
    AEMBBuildablePiece ..> UEMBStructureSubsystem : Register et RequestCollapse

    AEMBDragonAIController o-- AEMBDragonBoss : OnPossess
    AEMBDragonBoss ..> AEMBDragonAIController : PushStateToBlackboard
    AEMBDragonBoss ..> UEMBCombatComponent : ApplyHitToActor
    AEMBDragonArena ..> AEMBDragonBoss : OnPhaseChanged
    AEMBDragonArena ..> UEMBCombatComponent : ApplyHitToActor lave et foudre
    AEMBDragonArena ..> AEMBDragonBoss : Multicast_ShowGroundMarker (foudre)
    BTTask_EMBSelectAttack ..> AEMBDragonBoss
    BTTask_EMBDragonBreath ..> AEMBDragonBoss
    BTTask_EMBDragonDiveAttack ..> AEMBDragonBoss
    BTService_EMBUpdateCombatContext ..> AEMBDragonAIController
    EMBAnimNotifyState_HitWindow ..> UEMBCombatComponent : FindFromMesh
    EMBAnimNotifyState_ComboWindow ..> UEMBCombatComponent : FindFromMesh
    EMBAnimNotifyState_IFrames ..> UEMBCombatComponent : FindFromMesh

    UEMBProgressionSubsystem ..> FEMBGlyphRow : lit
    UEMBProgressionSubsystem ..> FEMBSkillNodeRow : lit
    UEMBProgressionSubsystem ..> UEMBProgressionComponent : GrantTrigger, AddXP (serveur)
    AEMBDragonBoss ..> UEMBProgressionSubsystem : NotifyWorldEvent à la mort (serveur)
    AEMBDragonBoss ..> UEMBProgressionSubsystem : IsWorldEventRecorded (BeginPlay)
```

Le détail de la progression et de la sauvegarde (réglages, GameMode, sous-système de sauvegarde, structures sauvegardées) est dans le diagramme dédié du § 2.7, pour garder celui-ci lisible.

**Remarques sur le diagramme**
- Le personnage **n'implémente pas** `IEMBHitReceiver` (commentaire explicite de `EMBPlayerCharacter.h`) : les coups qui le visent passent par son `UEMBCombatComponent`.
- `AEMBBuildablePiece` implémente désormais `IEMBHitReceiver` : `ReceiveEMBHit` applique un multiplicateur par type (physique ×1,0 ; feu ×1,5 sur le bois, ×0,5 sinon ; autres ×0,5) puis `ApplyStructuralDamage`. `TakeDamage` (dégâts moteur) est aussi relié à `ApplyStructuralDamage`.
- Le personnage substitue `UEMBCharacterMovementComponent` au mouvement par défaut (constructeur `FObjectInitializer`) ; le souhait de sprint voyage dans les drapeaux compressés de chaque mouvement (`FLAG_Custom_0`).
- Les notifies d'animation sont des assets partagés : ils ne mémorisent pas le composant et le retrouvent à chaque appel via `UEMBCombatComponent::FindFromMesh` (`FindComponentByClass` sur le propriétaire du mesh).

### 2.2 Machine à états du combat (`UEMBCombatComponent::CanTransitionTo`)

Transcription fidèle de la table de transitions du code (`EMBCombatComponent.cpp`, `CanTransitionTo`) et des déclencheurs (`TransitionTo`, `EnterState`, `OnActionFinished`, `EndHitWindow`, `ReleaseCharge`). La FSM ne tourne que sur le serveur.

```mermaid
stateDiagram-v2
    [*] --> Idle

    state "Vivant" as Vivant {
        Idle --> LightAttack : léger (14 endurance)
        Idle --> HeavyAttack : lourde (26)
        Idle --> ChargedAttack : début de charge (jauge > 0)
        Idle --> Dodge : roulade (18 ou 22)
        Idle --> Parry : parade (10)
        Idle --> Block : garde maintenue (ou reprise auto si bBlockHeld)

        Block --> Idle : garde relâchée
        Block --> LightAttack : action depuis la garde
        Block --> HeavyAttack : action depuis la garde
        Block --> ChargedAttack : action depuis la garde
        Block --> Dodge : action depuis la garde
        Block --> Parry : action depuis la garde

        LightAttack --> Recovery : fin de HitWindow (frame 18)
        LightAttack --> Idle : fin du montage
        HeavyAttack --> Recovery : fin de HitWindow (frame 32)
        HeavyAttack --> Idle : fin du montage

        ChargedAttack --> HeavyAttack : relâché avant 0,4 s (bCharging)
        ChargedAttack --> Dodge : roulade pendant la charge (bCharging, endurance de charge non dépensée)
        ChargedAttack --> Recovery : fin de HitWindow de la frappe (relâchement manuel ou automatique à 1,2 s)
        ChargedAttack --> Idle : fin de la frappe ou endurance vide au relâchement

        Recovery --> LightAttack : fenêtre de combo ouverte et dernière attaque légère
        Recovery --> HeavyAttack : fenêtre de combo ouverte et dernière attaque légère
        Recovery --> Idle : fin du montage

        Dodge --> Idle : fin du montage (0,70 s)
        Dodge --> LightAttack : annulation dès 0,50 s
        Dodge --> HeavyAttack : annulation dès 0,50 s
        Dodge --> ChargedAttack : annulation dès 0,50 s
        Dodge --> Parry : annulation dès 0,50 s
        Dodge --> Dodge : annulation dès 0,50 s
        Dodge --> Block : annulation dès 0,50 s

        Parry --> Idle : fin du montage (0,75 s)

        Idle --> Staggered : poise brisée
        Block --> Staggered : garde brisée (1,2 s)
        LightAttack --> Staggered : poise brisée
        HeavyAttack --> Staggered : poise brisée
        ChargedAttack --> Staggered : poise brisée
        Recovery --> Staggered : poise brisée
        Dodge --> Staggered : poise brisée
        Parry --> Staggered : poise brisée
        Staggered --> Idle : timer 0,8 s ou 1,2 s
    }

    Vivant --> Dead : OnDeath (PV = 0), depuis tout état
    Dead --> [*]
```

Règles transversales codées :
- `Dead` : aucune sortie ; on peut y entrer depuis n'importe quel état.
- `Staggered` : accessible depuis tout état sauf `Staggered` lui-même (« pas de stun-lock ») et `Dead`. Durée pilotée par un timer (0,8 s poise brisée, 1,2 s garde brisée), pas par le montage.
- `Dodge --> Dodge` : autorisé par la table (une action de départ depuis `Dodge` quand `bDodgeCancelOpen`), donc une roulade peut en enchaîner une autre après 0,50 s.
- `ChargedAttack --> Dodge` : seule annulation possible **pendant la charge** (`bCharging` vrai) ; l'endurance de la charge (38) n'étant facturée qu'au relâchement, rien n'est dépensé. Une fois la frappe lancée, plus d'annulation. Cette roulade n'est **pas prédite** côté client (la prédiction n'agit que depuis `Idle`, `Block` ou la fenêtre de combo) : elle part au retour du serveur.
- **Relâchement automatique** : un timer de 1,2 s (`ChargedMaxChargeTime`) armé à l'entrée en `ChargedAttack` appelle `ReleaseCharge` si le joueur maintient encore le bouton.
- Roulade **interdite** si la charge portée dépasse 100 % (`EncumbranceRatio > LoadHeavyMax`) ; coût 22 au-delà de 70 % (18 sinon).
- `Recovery` n'autorise **ni roulade ni parade** : un input de ce type est mémorisé (buffer 0,2 s) et exécuté au retour en `Idle`.
- Le combo exige `LastAttackState == LightAttack` : la `Recovery` d'une attaque lourde ou chargée ne s'enchaîne pas.
- Au retour en `Idle`, le serveur consomme l'input mémorisé puis relève la garde si `bBlockHeld` est encore vrai.
- Tags d'état : `State.Combat.Invulnerable` (compteur `InvulnerableCount` ; pour la roulade, **timer serveur** de la frame 3 à la frame 16, 14 ou 12 selon la charge portée — 13, 11 ou 9 frames d'i-frames —, indépendant du montage et de l'évaluation d'animation ; le notify `IFrames` reste possible mais optionnel et cumulatif), `State.Combat.HyperArmor` (lourde : timer frames 6-31 ; chargée : dès la charge jusqu'à la fin de la fenêtre active), `State.Combat.Staggered` et `State.Dead` (déduits de `CurrentState`). Les compteurs sont remis à zéro à chaque transition (`ResetTransientFlags`).
- La fenêtre de parade parfaite (frames 4-13) et la fenêtre de riposte (1,5 s après une parade parfaite, `IsRiposteWindowOpen`) sont aussi des timers / horodatages serveur.

### 2.3 Séquence « le joueur attaque le dragon »

```mermaid
sequenceDiagram
    autonumber
    actor J as Joueur (client propriétaire)
    participant PC as AEMBPlayerCharacter (client)
    participant CCc as UEMBCombatComponent (client)
    participant CCs as UEMBCombatComponent (serveur)
    participant ST as UEMBStatsComponent joueur (serveur)
    participant N as Notify HitWindow (serveur)
    participant D as AEMBDragonBoss (serveur)
    participant DS as UEMBStatsComponent dragon (serveur)
    participant AI as AEMBDragonAIController (serveur)
    participant O as Autres clients

    J->>PC: Enhanced Input LightAttackAction (Started)
    PC->>CCc: SendCombatInput(LightAttack) sauf en mode construction
    CCc->>CCc: PredictLocally : joue le montage si Idle, Block ou fenêtre de combo
    CCc->>CCs: Server_HandleInput(LightAttack) RPC Reliable WithValidation
    CCs->>CCs: ProcessInput : anti-flood 30 inputs/s puis ExecuteInput
    CCs->>CCs: TryStartLightAttack : CanTransitionTo(LightAttack)
    CCs->>ST: ConsumeStamina(14)
    alt Refusé (état incompatible ou endurance vide)
        CCs->>CCs: BufferInput (0,2 s) si bufferisable
        CCs-->>CCc: Client_RejectPrediction si ni exécuté ni mémorisé
    else Accepté
        CCs->>CCs: TransitionTo(LightAttack) puis BeginAttackAction
        CCs->>O: Multicast_PlayMontage (Unreliable)
        Note over CCc: le propriétaire ignore le multicast si le montage prédit tourne déjà
        CCs-->>CCc: CurrentState répliqué puis OnRep_CurrentState
        N->>CCs: BeginHitWindow puis TickHitWindow à chaque tick d'animation
        CCs->>CCs: SweepBlade (SweepMultiByChannel sur EMBMeleeHit, sous-pas, meilleur impact avec os, 1 coup par acteur)
        opt Fenêtre de riposte ouverte et cible = attaquant paré
            CCs->>CCs: Damage x3 (CriticalRiposteMultiplier), fenêtre refermée si Hit ou Killed
        end
        CCs->>D: ApplyHitToActor : cast IEMBHitReceiver puis ReceiveEMBHit
        D->>D: ignoré si transition de phase ou mort
        D->>D: BeginEncounter (idempotent) : mise à l'échelle coop figée avant tout dégât
        D->>D: multiplicateur os (ResolveWeakPointMultiplier), x3 si étourdi, x1,5 si vulnérable
        D->>DS: ApplyDamage(dégâts finaux)
        D->>D: AddThreat(instigateur)
        D->>DS: ApplyPoiseDamage si non étourdi
        opt Posture brisée
            DS-->>D: OnPoiseBroken puis HandlePoiseBroken : bIsStunned, timer 4 s
            D->>AI: PushStateToBlackboard puis SyncBlackboardState
            Note over AI: bIsStunned = true, observer aborts : la tâche en cours est abandonnée
        end
        D->>D: EvaluatePhaseThresholds : StartPhaseTransition à 70 % et 35 %
        D-->>CCs: résultat (Hit ou Killed)
        CCs-->>CCc: Client_ConfirmHit(cible, résultat, point) Unreliable puis OnHitConfirmed
        DS-->>O: Health et RepPoise répliqués puis OnAttributeChanged (barre de vie du boss)
        N->>CCs: EndHitWindow
        CCs->>CCs: TransitionTo(Recovery), fenêtre de combo gardée ouverte
        CCs->>CCs: fin du montage : OnActionFinished puis Idle
    end
```

### 2.4 Séquence « le joueur pose une pièce »

```mermaid
sequenceDiagram
    autonumber
    actor J as Joueur (client propriétaire)
    participant BCc as UEMBBuildingComponent (client)
    participant SSc as UEMBStructureSubsystem (client)
    participant BCs as UEMBBuildingComponent (serveur)
    participant INV as UEMBInventoryComponent (serveur)
    participant P as AEMBBuildablePiece (serveur)
    participant SSs as UEMBStructureSubsystem (serveur)
    participant O as Clients à portée (NetCullDistance 150 m)

    J->>BCc: ToggleBuildMode (au repos uniquement) puis SetSelectedPiece(ligne)
    loop Aperçu 30 Hz (TickComponent, client local)
        BCc->>BCc: ComputeDesiredTransform (grille, sockets Snap_, sol)
        BCc->>SSc: PredictIntegrity (lit l'intégrité répliquée des voisins)
        BCc->>BCc: EvaluatePlacement(bPreview = vrai) puis couleur du fantôme
    end
    J->>BCc: ConfirmPlacement (attaque légère ou action dédiée)
    BCc->>BCc: UpdatePreview frais, refus local si non Valid
    BCc->>BCs: Server_PlacePiece(PieceRowName, Transform) RPC Reliable WithValidation
    Note over BCs: Validate rejette seulement NaN, nom vide, coordonnées hors monde
    BCs->>BCs: anti-spam MinServerPlaceInterval 0,1 s, sinon RateLimited
    BCs->>BCs: FindRowCopy dans la table SERVEUR, ResolvePieceClass
    BCs->>BCs: SanitizeTransform (lacet seul, pas quantifié, échelle 1)
    BCs->>BCs: refus silencieux si le joueur est mort (état Dead)
    BCs->>BCs: EvaluatePlacement(bPreview = faux) : distance + 150 cm, grille de la catégorie (300/150/25 cm, ±5 cm) ou socket, sol, pente, chevauchement (canal EMBBuilding), zone interdite, limites 15 000/monde et 3 000/base, intégrité, ressources
    alt Invalide
        BCs-->>BCc: Client_PlacementResult(motif, dont LimitReached)
    else Valide
        BCs->>INV: ConsumeResources(Row.Cost) atomique
        BCs->>P: SpawnActorDeferred puis InitializePiece(..., PlacerPlayerId) puis FinishSpawning
        P->>SSs: BeginPlay : RegisterPiece (adjacence, intégrité, propagation)
        SSs->>P: SetIntegrityServer (FlushNetDormancy si la valeur change)
        P->>P: SetNetDormancy(DORM_DormantAll)
        P-->>O: bunch initial (identité et PlacerPlayerId COND_InitialOnly, PV, intégrité)
        O->>O: BeginPlay client : RegisterPiece dans le graphe local
        INV-->>BCc: Resources répliqué (OwnerOnly) puis OnInventoryChanged
        BCs-->>BCc: Client_PlacementResult(Valid, intégrité prédite)
        BCc->>BCc: OnPlacementResult (retour UI)
    end
```

### 2.5 Séquence « une pièce est détruite » (dégâts puis effondrement en cascade)

```mermaid
sequenceDiagram
    autonumber
    participant SRC as Source du coup (lame du joueur, TakeDamage moteur)
    participant P as AEMBBuildablePiece (serveur)
    participant SS as UEMBStructureSubsystem (serveur)
    participant V as Pièces dépendantes (serveur)
    participant O as Clients à portée

    SRC->>P: ReceiveEMBHit via ApplyHitToActor (multiplicateur par type et matériau), ou TakeDamage (direct)
    P->>P: ApplyStructuralDamage : PV réduits, FlushNetDormancy + ForceNetUpdate
    P-->>O: CurrentHealth répliqué puis UpdateVisualState
    opt PV à 0
        P->>SS: Collapse puis RequestCollapse(pièce)
        Note over SS: demandes de la frame regroupées (SetTimerForNextTick)
        SS->>SS: FlushDirectCollapses : UN RemoveBatch pour tout le lot
        SS->>SS: CollectDependentNodes : sous-arbre des pièces dont le support parent passait par le lot
        SS->>P: ExecuteCollapse (débris lourds pour les 8 premières pièces du lot au plus)
        P-->>O: bCollapsed répliqué + Multicast_PlayCollapseEffects (Unreliable)
        SS->>SS: RecomputeAffected : remise à zéro, réamorçage depuis les voisins non affectés, Dijkstra local
        SS->>V: SetIntegrityServer (réveil de dormance si la valeur change)
        SS->>SS: EnqueueUnstable : pièces à intégrité 0 triées par niveau (profondeur BFS) puis distance
        loop Toutes les 0,08 s tant que la file n'est pas vide
            SS->>V: ProcessCollapseWave : 32 pièces au plus, RemoveBatch sans recalcul, ExecuteCollapse
        end
    end
```

Une pièce mise en file mais « sauvée » entre-temps (support posé pendant la cascade, intégrité > 0) est ignorée par la vague. Les vagues ne refont **aucun** recalcul : une pièce non soutenue ne soutient personne.

### 2.6 Séquence « le joueur sprinte »

```mermaid
sequenceDiagram
    autonumber
    actor J as Joueur (client propriétaire)
    participant PC as AEMBPlayerCharacter
    participant MC as UEMBCharacterMovementComponent (client)
    participant MS as UEMBCharacterMovementComponent (serveur)
    participant PS as AEMBPlayerCharacter (serveur)
    participant ST as UEMBStatsComponent (serveur)

    J->>PC: SprintAction Started (refusé si endurance vide ou mode construction)
    PC->>MC: SetWantsToSprint(true) : GetMaxSpeed = SprintSpeed, prédit
    MC->>MS: mouvements client avec FLAG_Custom_0 (FEMBSavedMove)
    MS->>MS: UpdateFromCompressedFlags : même vitesse rejouée côté serveur
    MS->>PS: NotifySprintRequested(true) au changement de souhait
    PS->>ST: timer 0,2 s : DrainSprintStamina (8 endurance/s)
    alt Endurance vide, Staggered ou Dead
        PS->>MS: ForbidSprint (jusqu'au relâchement de la touche)
        PS-->>PC: Client_StopSprint (Reliable) puis SetWantsToSprint(false)
    end
    J->>PC: SprintAction Completed puis SetWantsToSprint(false)
    MS->>PS: NotifySprintRequested(false) : arrêt du timer
```

### 2.7 Diagramme de classes « progression et sauvegarde »

```mermaid
classDiagram
    direction LR

    class AGameModeBase
    class APlayerState
    class UActorComponent
    class UWorldSubsystem
    class UGameInstanceSubsystem
    class UDeveloperSettings
    class USaveGame
    class AActor

    class AEMBGameMode {
        +InitGame(MapName, Options, Error)
        +StartPlay()
        +PostLogin(APlayerController)
        +Logout(AController)
        +EndPlay(EEndPlayReason)
    }
    class AEMBPlayerState {
        -UEMBProgressionComponent Progression
        +GetProgressionComponent() UEMBProgressionComponent
        #CopyProperties(APlayerState)
        #OverrideWith(APlayerState)
    }
    class UEMBProgressionComponent {
        -int32 Level
        -int64 TotalXP
        -int32 UnspentGlyphPoints
        -int32 UnspentAwakeningPoints
        -TArray~FName~ UnlockedGlyphs
        -TArray~FName~ RevealedGlyphs
        -TArray~FEMBSkillRank~ SkillRanks
        -TArray~FName~ Knowledge
        -FGameplayTagContainer ObtainedTriggers
        -FGameplayTagContainer GrantedTags
        -FEMBPassiveModifiers Modifiers
        +AddXP(int64)
        +GrantTrigger(FGameplayTag)
        +GrantKnowledge(FName)
        +GetGlyphState(FName) EEMBGlyphState
        +CanUnlockGlyph(FName, FText) bool
        +RequestUnlockGlyph(FName)
        +CanRankUpSkill(FName, FText) bool
        +RequestRankUpSkill(FName)
        +HasGlyph(FName) bool
        +HasGrantedTag(FGameplayTag) bool
        +ApplyModifiersToPawn(APawn)
        +ExportSaveData(FEMBProgressionSaveData)
        +ImportSaveData(FEMBProgressionSaveData)
        -Server_UnlockGlyph(FName)
        -Server_RankUpSkill(FName)
        +OnXPChanged
        +OnLevelUp
        +OnPointsChanged
        +OnGlyphStateChanged
        +OnSkillRankChanged
    }
    class UEMBProgressionSubsystem {
        -UDataTable GlyphTable
        -UDataTable SkillTable
        -FGameplayTagContainer WorldEvents
        +NotifyWorldEvent(FGameplayTag, AActor, int64, float)
        +NotifyPlayerEvent(APlayerState, FGameplayTag, int64)
        +FindGlyph(FName) FEMBGlyphRow
        +FindSkill(FName) FEMBSkillNodeRow
        +IsWorldEventRecorded(FGameplayTag) bool
        +RestoreWorldEvents(FGameplayTagContainer)
        +GetWorldEvents() FGameplayTagContainer
        +OnWorldEventRecorded
    }
    class UEMBProgressionSettings {
        <<Config=Game>>
        +GlyphTable
        +SkillTable
        +EEMBSecretGlyphMode SecretGlyphMode
        +EEMBDeathSaveBehavior DeathSaveBehavior
        +XPBase, XPExponent, MaxLevel
        +GlyphPoints par tranche, AwakeningPoints
        +GuardianXP, SeraphXP, DragonXP
        +float ParticipantRadius
        +Get()$ UEMBProgressionSettings
        +GetXPForNextLevel(int32) int64
        +GetCumulativeXPForLevel(int32) int64
        +GetGlyphPointsForLevel(int32) int32
    }
    class AEMBDiscoveryVolume {
        +FGameplayTag DiscoveryTag
        +FName GrantedKnowledge
        +int64 XPReward
        -UBoxComponent Box
    }
    class UEMBSaveSubsystem {
        <<Config=Game>>
        +float AutosaveIntervalSeconds
        +float SafePointIntervalSeconds
        +float PawnRestoreTimeoutSeconds
        +BuildPiecesTable
        -TMap PlayerCache
        -TMap LastSafeTransforms
        -TMap ControllerIds
        -TMap ActiveIds
        +BindToWorld(UWorld)
        +SetActiveSlot(int32)
        +LoadActiveSlot()
        +RestorePlayer(APlayerController)
        +CapturePlayer(APlayerController)
        +RequestSave(bool)
        +SaveNowBlocking()
        +GetStablePlayerId(APlayerState)$ FString
        +IsLoaded() bool
        +IsWriteBlocked() bool
        +OnSaveCompleted
        +OnLoadCompleted
    }
    class UEMBSaveGame {
        +int32 CurrentFormatVersion$
        +int32 SaveFormatVersion
        +FDateTime SaveTimestamp
        +int32 SlotIndex
        +FEMBWorldSaveData World
        +TArray~FEMBPlayerSaveData~ Players
        +TArray~FEMBPieceSaveData~ Pieces
        +int32 ExpectedPlayerCount
        +int32 ExpectedPieceCount
        +bool bComplete
        +FinalizeForWrite()
        +IsComplete() bool
        +MigrateToCurrent() bool
        +IsNewerThan(UEMBSaveGame) bool
    }
    class FEMBPlayerSaveData {
        <<struct>>
        StableId, Progression
        bHasPawnState, Resources, Health, Stamina
        bHasPawnTransform, PawnTransform
        bHasLastSafeTransform, LastSafeTransform
    }
    class FEMBPieceSaveData {
        <<struct>>
        SaveId, RowName, PieceClassPath, Transform
        Health, Material, Category, bAnchoredToGround
        PlacerStableId, SupportLinkIds
    }
    class FEMBWorldSaveData {
        <<struct>>
        WorldEvents
    }
    class FEMBProgressionSaveData {
        <<struct>>
        Level, TotalXP, points, glyphes
        SkillRanks, Knowledge, ObtainedTriggers
    }

    AGameModeBase <|-- AEMBGameMode
    APlayerState <|-- AEMBPlayerState
    UActorComponent <|-- UEMBProgressionComponent
    UWorldSubsystem <|-- UEMBProgressionSubsystem
    UGameInstanceSubsystem <|-- UEMBSaveSubsystem
    UDeveloperSettings <|-- UEMBProgressionSettings
    USaveGame <|-- UEMBSaveGame
    AActor <|-- AEMBDiscoveryVolume

    AEMBPlayerState *-- UEMBProgressionComponent
    UEMBSaveGame *-- FEMBWorldSaveData
    UEMBSaveGame *-- FEMBPlayerSaveData
    UEMBSaveGame *-- FEMBPieceSaveData
    FEMBPlayerSaveData *-- FEMBProgressionSaveData

    AEMBGameMode ..> UEMBSaveSubsystem : cycle de vie (serveur)
    AEMBGameMode ..> AEMBPlayerState : PlayerStateClass
    UEMBProgressionComponent ..> UEMBProgressionSubsystem : FindGlyph, FindSkill
    UEMBProgressionComponent ..> UEMBProgressionSettings : barèmes
    UEMBProgressionSubsystem ..> UEMBProgressionSettings : chemins des tables
    UEMBProgressionSubsystem ..> UEMBProgressionComponent : GrantTrigger, AddXP
    AEMBDiscoveryVolume ..> UEMBProgressionSubsystem : NotifyPlayerEvent
    AEMBDiscoveryVolume ..> UEMBProgressionComponent : GrantKnowledge
    UEMBSaveSubsystem ..> UEMBProgressionSubsystem : OnWorldEventRecorded, RestoreWorldEvents
    UEMBSaveSubsystem ..> UEMBProgressionComponent : ExportSaveData, ImportSaveData
    UEMBSaveSubsystem ..> UEMBSaveGame : AsyncSave / AsyncLoad
    UEMBSaveSubsystem ..> UEMBStructureSubsystem : BeginBulkLoad, EndBulkLoad, GetAllPieces
    UEMBSaveSubsystem ..> AEMBBuildablePiece : SpawnActorDeferred, RestoreFromSave
    UEMBSaveSubsystem ..> AEMBDragonBoss : IsEncounterActive, ApplyDefeatedFromSave
    UEMBSaveSubsystem ..> UEMBInventoryComponent : ExportResources, ImportResources
    UEMBSaveSubsystem ..> UEMBStatsComponent : RestoreFromSave
    UEMBSaveSubsystem ..> UEMBProgressionSettings : DeathSaveBehavior
```

**Remarques**
- Le **sous-système de sauvegarde vit dans le GameInstance** (il survit aux changements de niveau) mais n'agit que sur le **serveur** et sur le monde auquel il est lié (`BindToWorld`) : chaque minuterie et chaque callback asynchrone vérifient `IsServer()` et un jeton de génération (`LoadGeneration`) qui invalide les réponses d'un monde précédent.
- Le **sous-système de progression est créé sur toutes les machines** (`ShouldCreateSubsystem` : mondes de jeu, serveur et clients), car l'UI et `IsPieceUnlocked` y lisent les tables ; la distribution (XP, déclencheurs) et `WorldEvents` n'existent que sur le serveur. Écart mineur avec le référentiel § 7.1 (« actif sur le serveur ») : seul le comportement est serveur.
- Les tables sont chargées **de façon synchrone** à l'initialisation du monde (`LoadTables`), avec contrôle de la structure de ligne.
- `FEMBResourceStack` est un alias (`using`) de `FEMBResourceEntry` : la sauvegarde réutilise la paire de l'inventaire, dont les champs sont marqués `SaveGame`.

### 2.8 Séquence « achat d'un glyphe »

```mermaid
sequenceDiagram
    autonumber
    actor J as Joueur (client propriétaire)
    participant UI as Arbre de glyphes (UMG, prévu)
    participant PCc as UEMBProgressionComponent (client)
    participant PCs as UEMBProgressionComponent (serveur)
    participant SUB as UEMBProgressionSubsystem (serveur)
    participant PAWN as Composants du pawn (serveur)
    participant PS as AEMBPlayerState

    UI->>PCc: GetGlyphState / CanUnlockGlyph(GlyphId, raison) : affichage (lecture locale de l'état répliqué)
    J->>UI: clic sur un glyphe Available
    UI->>PCc: RequestUnlockGlyph(GlyphId)
    PCc->>PCs: Server_UnlockGlyph(GlyphId) RPC Server Reliable WithValidation
    Note over PCs: Validate renvoie toujours vrai (un identifiant périmé n'est pas une triche, un Validate faux déconnecte)
    PCs->>SUB: FindGlyph(GlyphId)
    PCs->>PCs: CanUnlockGlyph : ligne existante, ni Unlocked ni Hidden, niveau, parents, savoirs, déclencheur secret, points
    alt Refusé
        PCs->>PCs: journal Verbose seulement (aucun retour au client)
    else Accepté
        PCs->>PCs: UnlockGlyphInternal : points débités, ajout à UnlockedGlyphs, OnPointsChanged (serveur)
        PCs->>PCs: RefreshAll : GrantedTags et Modifiers recalculés, états de glyphes comparés, délégués
        PCs->>PAWN: PushModifiersToCurrentPawn : SetPassiveModifiers, SetBuildCostMultiplier, SetCarryWeightBonus
        PCs->>PS: MarkNetDirty : ForceNetUpdate (PlayerState à 10 Hz)
        PCs-->>PCc: UnspentGlyphPoints, UnlockedGlyphs répliqués (COND_OwnerOnly)
        PCc->>PCc: OnRep_Points puis OnPointsChanged, OnRep_Glyphs puis RebuildDerivedState et RefreshGlyphStates
        PCc-->>UI: OnGlyphStateChanged(GlyphId, Unlocked) et glyphes enfants devenus Available
    end
```

Le rang de compétence suit exactement le même chemin (`RequestRankUpSkill` → `Server_RankUpSkill` → `CanRankUpSkill` : ligne, rang maximal, niveau, parents au rang ≥ 1, savoirs, points d'Éveil) ; un rang de passif modifie `Modifiers` et part immédiatement vers le pawn. Une pièce dont la ligne porte un `RequiredGlyph` devient constructible dès que `HasGlyph` est vrai, côté aperçu (client) comme côté validation (serveur).

### 2.9 Séquence « mort du dragon → XP, déclencheur, autosave »

```mermaid
sequenceDiagram
    autonumber
    participant DS as UEMBStatsComponent dragon (serveur)
    participant D as AEMBDragonBoss (serveur)
    participant SUB as UEMBProgressionSubsystem (serveur)
    participant P as UEMBProgressionComponent de chaque participant (serveur)
    participant SAV as UEMBSaveSubsystem (serveur)
    participant AR as AEMBDragonArena (serveur et clients)
    participant C as Clients

    DS-->>D: OnDeath puis HandleStatsDeath(tueur)
    D->>D: bIsDead = vrai, timers coupés, montage de mort, BrainComponent arrêté
    D->>D: OnBossDefeated.Broadcast(Event.Boss.Killed.Dragon) (aucun abonné C++)
    D->>SUB: NotifyWorldEvent(Event.Boss.Killed.Dragon, dragon, DragonXP 60 000, ParticipantRadius 10 000 cm)
    alt Événement déjà enregistré (boss déjà vaincu)
        SUB-->>D: rien (pas d'XP redonnée)
    else Première fois
        loop Chaque PlayerState du GameState dont le pawn (vivant ou mort) est à moins de 10 000 cm
            SUB->>P: GrantTrigger : ObtainedTriggers, +1 point d'Éveil (boss), glyphes secrets révélés ou débloqués (SecretGlyphMode)
            SUB->>P: AddXP(60 000) si déclencheur nouveau : niveaux multiples, points de glyphe et d'Éveil
            P-->>C: Level, TotalXP (tous), points, glyphes, déclencheurs (propriétaire seul)
        end
        SUB->>SUB: WorldEvents.AddTag APRÈS la distribution
        SUB->>SAV: OnWorldEventRecorded
        SAV->>SAV: RequestSave(false) : IsEncounterActive faux (bIsDead) donc écriture immédiate
        SAV->>SAV: BuildSnapshot puis AsyncSaveGameToSlot (fichier A ou B suivant)
    end
    D->>AR: OnEncounterEnded : EnterFinalCalmState (lave, foudre et tempête arrêtées, OnArenaFinalCalmState)
    D-->>C: bIsDead répliqué puis OnRep_IsDead : OnEncounterEnded côté clients, arène calme
```

L'ordre « distribution puis enregistrement » est voulu : l'autosave déclenchée par `OnWorldEventRecorded` capture des progressions déjà à jour. Un joueur mort mais présent dans le rayon est participant (son pawn existe encore, aucune réapparition n'étant codée).

### 2.10 Séquence « chargement d'un monde »

```mermaid
sequenceDiagram
    autonumber
    participant GM as AEMBGameMode (serveur)
    participant SAV as UEMBSaveSubsystem
    participant DISK as Fichiers EMB_World_n_A / _B
    participant SUB as UEMBProgressionSubsystem
    participant D as AEMBDragonBoss
    participant ST as UEMBStructureSubsystem
    participant P as AEMBBuildablePiece (créées)
    participant PC as Joueur (PlayerController)

    GM->>SAV: InitGame : BindToWorld(World) (synchrone, remise à zéro, génération +1, timers recréés, écoute OnWorldEventRecorded)
    GM->>SAV: SetActiveSlot(?Slot=N, borné 0..2)
    Note over D: BeginPlay du dragon : CheckDefeatedFromSave (WorldEvents encore vide à ce stade)
    GM->>SAV: StartPlay : LoadActiveSlot (idempotent, LoadState = Loading)
    SAV->>DISK: AsyncLoadGameFromSlot(_A) et AsyncLoadGameFromSlot(_B)
    opt Un joueur se connecte pendant la lecture
        GM->>SAV: PostLogin : RestorePlayer : ClaimIdentity puis mise en attente (PendingRestoreControllers)
    end
    DISK-->>SAV: OnSlotLoaded x2 (callback ignoré si la génération a changé)
    SAV->>SAV: FinishLoading : pour chaque fichier, bon monde ? MigrateToCurrent ? IsComplete ?
    Note over SAV: version inconnue ou inférieure à 1 : écritures BLOQUÉES. Fichier incomplet, absent ou illisible : ignoré
    SAV->>SAV: retient le plus récent valide (IsNewerThan), prochaine écriture = l'autre fichier
    alt Aucun fichier valide
        SAV->>SAV: monde vierge
    else Sauvegarde retenue
        SAV->>SAV: ApplyLoadedSave : PlayerCache (256 joueurs max), derniers points sûrs
        SAV->>SUB: RestoreWorldEvents (sans diffusion)
        SAV->>D: ApplyDefeatedFromSave si Event.Boss.Killed.Dragon (caché, sans collision ni IA, OnEncounterEnded)
        SAV->>ST: BeginBulkLoad (adjacence seule, aucune intégrité ni effondrement)
        loop Chaque pièce (15 000 max)
            SAV->>SAV: ligne DT_BuildPieces existante et transform sain, classe, matériau et catégorie relus dans la TABLE
            SAV->>P: SpawnActorDeferred, InitializePiece(..., INDEX_NONE), RestoreFromSave(PV, PlacerStableId), FinishSpawning
            P->>ST: RegisterPiece (mode masse)
        end
        SAV->>ST: EndBulkLoad : UN recalcul global depuis les ancres, pièces à intégrité ≤ 0 détruites sans effet
        SAV->>SAV: contrôle des SupportLinkIds contre les voisins recalculés (journal seulement)
    end
    SAV->>SAV: LoadState = Loaded puis OnLoadCompleted(trouvé ou non)
    SAV->>PC: ProcessPendingRestores puis RestoreFromCache
    PC->>PC: ImportSaveData (progression, tout de suite, sur le PlayerState)
    alt Pawn déjà présent
        SAV->>PC: TryRestorePawn : ImportResources, Stats RestoreFromSave, TeleportTo (repli SetActorLocation)
    else Pawn pas encore créé
        SAV->>SAV: nouvelle tentative toutes les 0,25 s pendant 20 s, puis abandon (journal)
    end
```

Garde-fou central : **aucune écriture n'est possible avant `LoadState = Loaded`** (`RequestSave` et `SaveNowBlocking` l'ignorent), sinon un monde encore vide écraserait la vraie sauvegarde. Tant qu'une restauration de pawn est en attente, la capture du joueur ne lit pas le pawn neuf (`IsRestorePending`), pour ne pas écraser l'inventaire et les PV sauvegardés.

---

## 3. Communication entre systèmes

### 3.1 Délégués (liste exhaustive du code)

| Délégué (type) | Propriétaire | Diffusé où | Abonnés dans le code |
|---|---|---|---|
| `OnAttributeChanged` (`FEMBOnAttributeChanged` : type, valeur, max) | `UEMBStatsComponent` | serveur (à chaque modification) **et** clients (RepNotify `OnRep_Health/Stamina/Poise/MaxHealth`) | aucun (HUD prévu) |
| `OnDeath` (`FEMBOnDeath` : tueur) | `UEMBStatsComponent` | **serveur uniquement** | `UEMBCombatComponent::HandleOwnerDeath` → `Dead` ; `AEMBDragonBoss::HandleStatsDeath` |
| `OnPoiseBroken` (`FEMBOnPoiseBroken` : instigateur) | `UEMBStatsComponent` | **serveur uniquement** | `UEMBCombatComponent::HandleOwnerPoiseBroken` → `Staggered` 0,8 s ; `AEMBDragonBoss::HandlePoiseBroken` → étourdi 4 s |
| `OnCombatStateChanged` (`FEMBOnCombatStateChanged` : ancien, nouveau) | `UEMBCombatComponent` | serveur (`TransitionTo`) et clients (`OnRep_CurrentState`) | `AEMBPlayerCharacter::HandleCombatStateChanged` (arrêt du sprint, désactivation à la mort) |
| `OnParrySuccess` (`FEMBOnParrySuccess` : attaquant) | `UEMBCombatComponent` | partout via `Multicast_ParrySuccess` (Unreliable) | aucun (effets prévus) |
| `OnHitReceived` (`FEMBOnHitReceived` : coup, résultat) | `UEMBCombatComponent` | partout via `Multicast_BroadcastHitReceived` (Unreliable) ; **jamais** pour les dégâts continus (`bDamageOverTime`) | aucun (effets prévus) |
| `OnHitConfirmed` (`FEMBOnHitConfirmed` : cible, résultat, point d'impact) — **nouveau v1.1** | `UEMBCombatComponent` de l'**attaquant** | client propriétaire de l'attaquant (et hôte) via `Client_ConfirmHit` (Unreliable), pour tout coup de lame non `Ignored` (dragon, pièce, créature) | aucun (marqueur de coup, son, hit-stop prévus) |
| `OnInventoryChanged` (`FEMBOnInventoryChanged` : ressource, quantité) | `UEMBInventoryComponent` | serveur (ressource précise) ; propriétaire après réplication (`NAME_None` = tout rafraîchir) | `AEMBPlayerCharacter::HandleInventoryChanged` → `UpdateEncumbrance` → `SetEncumbranceRatio` (serveur et propriétaire) ; UI prévue |
| `OnBuildModeChanged` (`FEMBOnBuildModeChanged` : bool) | `UEMBBuildingComponent` | client local | aucun (UI prévue) |
| `OnPlacementResult` (`FEMBOnPlacementResult` : résultat, intégrité) | `UEMBBuildingComponent` | client local (aperçu) et réponse serveur (`Client_PlacementResult`) | aucun (UI prévue) |
| `OnPhaseChanged` (`FEMBOnDragonPhaseChanged` : nouvelle, ancienne) | `AEMBDragonBoss` | serveur (`FinishPhaseTransition`) et clients (`OnRep_CurrentPhase`) | `AEMBDragonArena::HandleBossPhaseChanged` |
| `OnPhaseTransitionStarted` (`FEMBOnDragonPhaseTransitionStarted` : phase visée) | `AEMBDragonBoss` | serveur et clients (`OnRep_IsTransitioning`, phase visée supposée = courante + 1) | aucun |
| `OnStunChanged` (`FEMBOnDragonStunChanged` : bool) | `AEMBDragonBoss` | serveur et clients (`OnRep_IsStunned`) | aucun |
| `OnBossDefeated` (`FEMBOnBossDefeated` : tag d'événement, tueur) | `AEMBDragonBoss` | **serveur uniquement**, tag `Event.Boss.Killed.Dragon` | aucun abonné C++ : la progression est appelée **directement** (`UEMBProgressionSubsystem::NotifyWorldEvent` dans `HandleStatsDeath`) ; le délégué reste disponible pour la mise en scène Blueprint |
| `OnEncounterEnded` (`FEMBOnDragonEncounterEnded`) — **nouveau v1.2** | `AEMBDragonBoss` | serveur (mort au combat, `ApplyDefeatedFromSave`) et clients (`OnRep_IsDead`) | `AEMBDragonArena::HandleEncounterEnded` → `EnterFinalCalmState` |
| `OnXPChanged` (`FEMBOnXPChanged` : XP totale, niveau) — **v1.2** | `UEMBProgressionComponent` | serveur (`AddXP`) et clients (`OnRep_Level`, `OnRep_TotalXP` : tous les clients) | aucun (UI prévue) |
| `OnLevelUp` (`FEMBOnLevelUp` : niveau) — **v1.2** | `UEMBProgressionComponent` | serveur (un appel par niveau gagné) et clients (`OnRep_Level`, sauf première réception) | aucun (UI prévue) |
| `OnPointsChanged` (`FEMBOnPointsChanged` : points de glyphe, points d'Éveil) — **v1.2** | `UEMBProgressionComponent` | serveur et propriétaire (`OnRep_Points`) | aucun (UI prévue) |
| `OnGlyphStateChanged` (`FEMBOnGlyphStateChanged` : glyphe, état) — **v1.2** | `UEMBProgressionComponent` | serveur et propriétaire ; le premier calcul est mémorisé **sans** diffusion | aucun (UI prévue) |
| `OnSkillRankChanged` (`FEMBOnSkillRankChanged` : nœud, rang) — **v1.2** | `UEMBProgressionComponent` | serveur et propriétaire (`OnRep_SkillRanks`), même règle de premier calcul | aucun (UI prévue) |
| `OnWorldEventRecorded` (`FEMBOnWorldEventRecorded` : tag) — **v1.2** | `UEMBProgressionSubsystem` | **serveur uniquement**, après distribution aux participants | `UEMBSaveSubsystem::HandleWorldEventRecorded` → `RequestSave(false)` |
| `OnSaveCompleted` (`FEMBOnSaveCompleted` : succès) — **v1.2** | `UEMBSaveSubsystem` | serveur, fin d'écriture (asynchrone ou bloquante) | aucun (UI prévue). **Pas** diffusé quand une demande est ignorée (chargement non terminé, écritures bloquées, rencontre de boss) |
| `OnLoadCompleted` (`FEMBOnLoadCompleted` : sauvegarde trouvée) — **v1.2** | `UEMBSaveSubsystem` | serveur, fin du chargement (faux = monde vierge) | aucun |

Événements Blueprint (`BlueprintImplementableEvent`, mise en scène) : `AEMBDragonBoss::BP_OnAttackTelegraph`, `BP_OnShowGroundMarker` ; `AEMBDragonArena::OnArenaPhaseApplied`, `OnAshStormChanged` (tempête de cendres de phase 3, purement visuelle), `OnArenaFinalCalmState` (**v1.2**, état final calme après la rencontre).

Délégués moteur utilisés : `UAIPerceptionComponent::OnTargetPerceptionUpdated` (contrôleur du dragon), `FOnMontageEnded` (fin d'action de la FSM, protégée par `ActionToken`), délégués de timers (dont `SetTimerForNextTick` pour le regroupement des destructions).

### 3.2 Gameplay Tags (`EMBTags`, `EMBGameplayTags.h`)

Tags natifs déclarés par `UE_DECLARE_GAMEPLAY_TAG_EXTERN` : utilisables sans `.ini`, à résoudre au `BeginPlay` ou à l'usage, jamais dans un constructeur de CDO (le code le respecte : `UEMBCombatComponent::ResolveDefaultTags` au `BeginPlay`).

| Famille | Tags | Utilisation réelle dans le code |
|---|---|---|
| `State.*` | `State.Combat.Invulnerable`, `.Staggered`, `.HyperArmor`, `State.Dead` | Joueur : `HasStateTag`, notify `IFrames` (tag vide = Invulnerable). Dragon : `StateTags` reconstruits localement (`RefreshStateTags`), exposés par `IGameplayTagAssetInterface`. |
| `Attack.Player.*` | `.Light`, `.Heavy`, `.Charged` | `FEMBAttackDefinition::AttackTag` → `FEMBHitData::AttackTag` |
| `Attack.Dragon.*` | 11 attaques | Clé fonctionnelle de `FEMBDragonAttackRow` (cache `AttackRowByTag`, recharges, historique anti-répétition) ; `Cataclysm` forcé par `BTTask_EMBDragonBreath::bUseCataclysmTag` |
| `Boss.Phase.*` | `1`, `2`, `3` | `AEMBDragonBoss::GetPhaseTag`, inclus dans `StateTags` |
| `Building.Material.*`, `Building.Category.*` | 4 + 8 tags | Déclarés ; le code utilise les **énumérations miroirs** `EEMBBuildMaterial` / `EEMBPieceCategory` |
| `Building.NoBuildZone` | 1 | Zone interdite (avec le tag d'acteur `NoBuild`) dans `UEMBBuildingComponent::IsNoBuildZone` |
| `Event.*` | `Event.Boss.Killed.Guardian/.Seraph/.Dragon`, `Event.Location.Discovered.SunkenLibrary/.AshAltar/.StarWell` | `Event.Boss.Killed.Dragon` : émis par le dragon (`NotifyWorldEvent`), enregistré dans `WorldEvents` et sauvegardé. `Event.Location.Discovered.*` : posés sur les `AEMBDiscoveryVolume` du niveau (`NotifyPlayerEvent`), enregistrés par joueur (`ObtainedTriggers`). Les trois tags de boss donnent un point d'Éveil (`IsBossKilledTag`) ; **Gardien et Séraphe n'ont aucun émetteur** (boss non codés). Déclencheurs des glyphes secrets : `FEMBGlyphRow::SecretTrigger` |
| `Glyph.*`, `Skill.*` (tags `.ini`, non natifs) | un tag par glyphe et par nœud (colonnes `GrantedTag` de `DT_Glyphs.csv` et `DT_SkillNodes.csv` ; déclarés dans `Config/Tags/EmberfallGlyphs.ini` et `EmberfallSkills.ini`, 20 tags `Skill.*`) | Agrégés dans `UEMBProgressionComponent::GrantedTags` (`HasGrantedTag`) ; **aucun système ne les lit encore** (recettes, compétences actives non codées). La construction se verrouille par **nom de glyphe** (`RequiredGlyph`, `HasGlyph`), pas par tag |

### 3.3 Interfaces

| Interface | Implémentée par | Rôle |
|---|---|---|
| `IEMBHitReceiver` (C++ natif, `CannotImplementInterfaceInBlueprint`) | `AEMBDragonBoss` | Traitement autonome d'un coup : invulnérabilité de transition, `BeginEncounter` avant tout dégât, points faibles par os (remontée de la hiérarchie du squelette), critique ×3 sur boss étourdi, ×1,5 pendant la vulnérabilité après le piqué, menace, posture (ignorée pendant l'étourdissement). Reçoit aussi la posture de parade (coup à 0 dégât, 35 de posture). |
| `IEMBHitReceiver` | `AEMBBuildablePiece` | Dégâts structurels : physique ×1,0 ; feu ×1,5 sur le bois, ×0,5 sur les autres matériaux ; autres types ×0,5. Retourne `Killed` si la pièce tombe à 0 PV. |
| `IGameplayTagAssetInterface` (moteur) | `AEMBDragonBoss` | Expose `Boss.Phase.N` et les tags d'état (lecture par l'UI, les décorateurs ou d'autres systèmes). |

### 3.4 Routage des dégâts : `UEMBCombatComponent::ApplyHitToActor`

Fonction **statique**, appelée uniquement sur le serveur (elle renvoie `Ignored` si `Target->HasAuthority()` est faux).

```
ApplyHitToActor(Target, Hit)
 1. Target implémente IEMBHitReceiver ?      -> ReceiveEMBHit(Hit)          (dragon, pièces de construction)
 2. sinon Target a un UEMBCombatComponent ?  -> ReceiveHit(Hit)             (joueur : esquive, parade, garde, dégâts)
 3. sinon Target a un UEMBStatsComponent ?   -> ApplyDamage (+ ApplyPoiseDamage sauf bDamageOverTime) (créatures simples, prévues)
 4. sinon                                    -> Ignored
```

Appelants réels : `UEMBCombatComponent::SweepBlade` (lame du joueur, avec bonus de riposte ×3 sur l'attaquant paré pendant 1,5 s, puis `Client_ConfirmHit`) ; `AEMBDragonBoss::ApplyHitToPawns` (zones et cônes), `TickBurn` (brûlure, `bDamageOverTime`), `TickFireZones` (zones de feu, `bDamageOverTime`) ; `AEMBDragonArena::TickLava` (lave, `bDamageOverTime`), `ResolveLightningImpact` (foudre de phase 3, coup normal esquivable, non parable).

Résolution côté joueur (`ReceiveHit`) : un coup **`bDamageOverTime`** (lave, brûlure, zones de feu) applique directement ses dégâts — ni esquive, ni garde, ni parade, ni counter-hit, ni multicast. Sinon `ResolveHit`, dans cet ordre : **i-frames** (`Dodged`) → **parade parfaite** de face, attaque parable (`Parried` ; ouvre la fenêtre de riposte ; `ApplyParryPosture` envoie 35 de posture à l'attaquant **via `ReceiveEMBHit`** s'il implémente `IEMBHitReceiver`, sinon directement sur ses jauges ; `Multicast_ParrySuccess`) → **garde** levée depuis 0,10 s, de face, attaque blocable (`Blocked` ; réduction par type : physique 85 %, feu/givre/foudre 50 %, corruption 30 % ; coût d'endurance `StaminaDamage` ou dégâts ×0,5 ; `GuardBroken` si la jauge tombe à 0) → **coup plein** (counter-hit ×1,25 en startup, recovery ou parade ratée ; poise ×0,5 sous hyper-armure). « De face » = direction vers l'**instigateur** dans un cône de `GuardHalfAngleDegrees` = 100° (demi-angle, hypothèse hors référentiel). Chaque résultat non ignoré part en `Multicast_BroadcastHitReceived`.

---

## 4. Behavior Tree du dragon (Vaelthyrax)

Les assets `BT_Vaelthyrax` et `BB_Vaelthyrax` **ne sont pas dans le dépôt** : ils sont à construire dans l'éditeur en suivant le schéma figé dans l'en-tête de `EMBDragonAIController.h`, repris ci-dessous. Le contrôleur n'existe que sur le serveur.

### 4.1 Arbre

```
Root
└─ Selector "Racine"                       [Service : BTService_EMBUpdateCombatContext (0,2 s ± 0,05)]
   │
   ├─ 1. Sequence "Mort"                   [Décorateur Blackboard : bIsDead == true ; Observer Aborts : Both]
   │     └─ Wait (durée très longue)       (le BrainComponent est de toute façon arrêté par le boss à la mort)
   │
   ├─ 2. Sequence "Transition de phase"    [Décorateur Blackboard : bIsTransitioning == true ; Observer Aborts : Both]
   │     └─ Wait 0,25 s                    (rugissement joué par le boss ; invulnérable ; aucune attaque)
   │
   ├─ 3. Sequence "Étourdi"                [Décorateur Blackboard : bIsStunned == true ; Observer Aborts : Both]
   │     └─ Wait 0,25 s                    (4 s d'étourdissement ; riposte critique x3 côté joueur)
   │
   ├─ 4. Sequence "Cataclysm scripté"      [Décorateur Blackboard : bCataclysmDue == true ; Observer Aborts : Lower Priority
   │     │                                  + BTDecorator_EMBDragonPhase (Min 3, Max 3)]
   │     └─ BTTask_EMBDragonBreath         (bUseCataclysmTag = true : 4 s de charge en vol, 900 dégâts, ligne de vue)
   │
   ├─ 5. Sequence "Combat"                 [Décorateur Blackboard : TargetActor is Set ; Observer Aborts : Self]
   │     ├─ BTTask_EMBSelectAttack         (échoue si aucune attaque valide -> la branche 6 prend le relais)
   │     ├─ Selector "Exécution par type"  (un décorateur Blackboard par enfant, sur la clé entière SelectedAttackKind)
   │     │    ├─ [SelectedAttackKind == 1 (Breath) ou == 5 (Scripted)]  BTTask_EMBDragonBreath (bUseCataclysmTag = false)
   │     │    ├─ [SelectedAttackKind == 2 (Dive)]                       BTTask_EMBDragonDiveAttack
   │     │    └─ [SelectedAttackKind == 0 (Melee), 3 (Area) ou 4 (Grab)]
   │     │         Wait Blackboard Time (clé : SelectedAttackDuration)  (impacts programmés par le boss)
   │     └─ Wait 0,3 à 0,6 s (aléatoire)   (récupération entre deux attaques : fenêtre de punition pour le joueur)
   │
   └─ 6. Sequence "Repositionnement"       (aucun décorateur : branche de repli)
         ├─ MoveTo (TargetActor, rayon d'acceptation 700 cm)
         └─ Wait 0,3 s                     (évite de boucler à chaque frame quand tout est en recharge)
```

Valeurs de `SelectedAttackKind` = `EEMBDragonAttackKind` : Melee 0, Breath 1, Dive 2, Area 3, Grab 4, Scripted 5. Remarque : `BTTask_EMBSelectAttack` **exclut** les attaques `bScripted` ; la valeur 5 n'atteint donc jamais la branche 5 (le Cataclysm passe uniquement par la branche 4). C'est sans danger.

### 4.2 Clés du Blackboard (`AEMBDragonAIController::BBKey_*`)

| Clé | Type | Écrite par | Lue par |
|---|---|---|---|
| `TargetActor` | Object (Actor) | `SetCombatTarget` (perception, service) | décorateur branche 5, `MoveTo`, tâches |
| `TargetDistance` | Float (cm, à plat) | service | `BTTask_EMBSelectAttack` |
| `TargetAngle` | Float (degrés, signé, > 0 = à droite) | service | `BTTask_EMBSelectAttack` |
| `CurrentPhase` | Int | `SyncBlackboardState` | `BTDecorator_EMBDragonPhase` |
| `bIsFlying` | Bool | `SyncBlackboardState` | (libre) |
| `bIsStunned` | Bool | `SyncBlackboardState` (poussée immédiate par le boss) | décorateur branche 3 |
| `bIsTransitioning` | Bool | idem | décorateur branche 2 |
| `bIsDead` | Bool | idem | décorateur branche 1 |
| `bIsVulnerable` | Bool | idem | aucune branche ne l'utilise aujourd'hui |
| `bCataclysmDue` | Bool | `SyncBlackboardState` (réévaluée par le service : dépend du temps) | décorateur branche 4 |
| `ThreatCount` | Int | service (joueurs à < 2 500 cm) | (libre) |
| `SelectedAttackTag` | Name | `BTTask_EMBSelectAttack` | `BTTask_EMBDragonBreath`, `BTTask_EMBDragonDiveAttack` |
| `SelectedAttackKind` | Int | `BTTask_EMBSelectAttack` | décorateurs du sélecteur d'exécution |
| `SelectedAttackDuration` | Float (s) | `BTTask_EMBSelectAttack` | `Wait Blackboard Time` |

Les **états critiques** (étourdi, transition, mort, vulnérable, phase) ne passent pas par l'intervalle du service : le boss appelle `PushStateToBlackboard` → `AEMBDragonAIController::SyncBlackboardState` au moment où l'état change, ce qui déclenche immédiatement les « observer aborts ».

### 4.3 Rôle de chaque nœud C++

| Nœud | Type | Rôle | Points techniques |
|---|---|---|---|
| `BTService_EMBUpdateCombatContext` | Service (0,2 s ± 0,05) | Synchronise l'état du boss, choisit la cible (et appelle `BeginEncounter`, idempotent, dès qu'une cible existe), écrit distance, angle et `ThreatCount`. | Score = proximité (1 au contact, 0 à `MaxTargetDistance` = 8 000 cm) + menace récente (demi-vie 8 s) ; bonus d'hystérésis ×1,15 sur la cible courante. Itère au plus 4 joueurs (`CollectAlivePlayerPawns`). |
| `BTTask_EMBSelectAttack` | Tâche instantanée (`bCreateNodeInstance`) | Tirage pondéré anti-répétition parmi les lignes de `DT_DragonAttacks`. | Score = poids de base × facteur distance × facteur angle × poids de phase × pénalité (×0,25 dernière, ×0,6 avant-dernière) ; `WingBuffet` ×3 si ≥ 2 joueurs à < 800 cm ; exclut recharges, scriptées, phases interdites. Lance directement les attaques Melee/Area/Grab (`BeginTimedAttack`, impacts par timers côté boss) et écrit leur durée. Échoue si aucune candidate. |
| `BTTask_EMBDragonBreath` | Tâche latente (`bNotifyTick`) | Souffles : `Cone` (20°, 2,5 s, visée figée en fin de télégraphe), `Sweep` (arc 120° en 2,0 s), `Aerial` (vol rasant, zones de feu 8 s à 20/s), `Cataclysm` (4 s de charge en vol, 900 dégâts avec ligne de vue). | 8 ticks de dégâts par seconde (`StaminaDamage` 10 par tick sur garde) ; télégraphe ×0,9 en phase 3 (sauf charge du Cataclysm) ; brûlure si `bAppliesBurn` ; souffle aérien : zones de feu serveur (`AddFireZone`) et **un** `Multicast_SpawnFireZoneVFX` (Unreliable) par traînée pour leur visuel ; nettoyage idempotent sur fin normale comme sur abandon ; appelle `NotifyCataclysmFired`. |
| `BTTask_EMBDragonDiveAttack` | Tâche latente (`bNotifyTick`) | Piqué en 4 étapes : Takeoff 2,0 s → Mark 1,5 s (ombre sur la position prédite, flash 0,35 s) → Dive 4 500 cm/s → impact rayon 600 → Vulnerable 2,5 s. | Lancement calé pour que l'impact tombe à la fin exacte de la marque ; `BeginVulnerableWindow` ; nettoyage idempotent (retour au sol). |
| `BTDecorator_EMBDragonPhase` | Décorateur Blackboard (clé `CurrentPhase`) | Vrai si `MinPhase ≤ phase ≤ MaxPhase`. | Observe la clé : ré-évaluation immédiate au changement de phase avec « Observer Aborts ». |

---

## 5. Progression (codée — v1.2)

Fichiers : `EMBProgressionTypes.h` (énumérations, `FEMBPassiveModifiers`, `FEMBSkillRank`, `FEMBProgressionSaveData`, lignes `FEMBGlyphRow` et `FEMBSkillNodeRow`), `EMBProgressionSettings.h/.cpp`, `EMBProgressionComponent.h/.cpp`, `EMBProgressionSubsystem.h/.cpp`, `EMBPlayerState.h/.cpp`, `EMBDiscoveryVolume.h/.cpp`. Référentiel : § 7 (v1.2).

### 5.1 ADR-P1 — Où vit l'état de progression ? (décision appliquée)

**Contexte.** Coop 1-4, serveur autoritaire. La progression (niveau, XP, points, glyphes, rangs, savoirs, déclencheurs) doit être décidée par le serveur, visible du propriétaire (UI de l'arbre), en partie visible des partenaires (niveau), survivre à la mort du pawn et être sauvegardée par l'hôte.

| Option | Avantages | Inconvénients |
|---|---|---|
| A. `UGameInstanceSubsystem` qui stocke l'état | Survit aux changements de carte | Une instance par machine, **jamais répliquée** : il faudrait réinventer la réplication par RPC. |
| B. Composant sur le **pawn** | Cohérent avec les autres composants | Le pawn est détruit à la mort / réapparition : état perdu ou à recopier. |
| C. **Composant sur `AEMBPlayerState`** + `UWorldSubsystem` sans état de joueur | Réplication native, existe sur toutes les machines, survit au pawn, conservé à la déconnexion (PlayerState inactif) | Exige `AEMBPlayerState` et `AEMBGameMode` (désormais codés). |

**Décision (codée) : C.** Conséquences constatées dans le code : le personnage ne dépend pas de la progression (c'est le composant qui **pousse** ses bonus vers le pawn) ; la construction lit `HasGlyph` via le PlayerState du pawn ; la sauvegarde exporte et importe une structure unique `FEMBProgressionSaveData`.

### 5.2 Rôles des classes

| Classe | Rôle | Machines |
|---|---|---|
| `UEMBProgressionSettings` (`UDeveloperSettings`, `Config=Game`) | Chemins des tables (`GlyphTable`, `SkillTable`), règles à valider (`SecretGlyphMode`, `DeathSaveBehavior`), barème XP, points, XP des boss, rayon des participants. Calculs : `GetXPForNextLevel(L) = round(XPBase × L^XPExponent)`, `GetCumulativeXPForLevel`, `GetGlyphPointsForLevel`. | toutes (objet par défaut) |
| `UEMBProgressionComponent` (sur `AEMBPlayerState`) | État par joueur, achats (RPC), XP et niveaux, déclencheurs, savoirs, état dérivé (`GrantedTags`, `Modifiers`, états de glyphes en cache), délégués d'UI, application des passifs au pawn, export/import. | serveur (autorité) + clients (lecture, UI) |
| `UEMBProgressionSubsystem` (`UWorldSubsystem`) | Tables chargées en synchrone (type de ligne contrôlé), recherche de lignes, **événements monde** (`WorldEvents`, serveur seul), distribution aux joueurs (`NotifyWorldEvent`, `NotifyPlayerEvent`), délégué `OnWorldEventRecorded`. | toutes (distribution : serveur) |
| `AEMBPlayerState` | Porte le composant, `NetUpdateFrequency` 10 Hz, copie de la progression au voyage transparent (`CopyProperties`) et à la reconnexion d'un joueur inactif (`OverrideWith`), par export/import (donc assainie). | toutes |
| `AEMBDiscoveryVolume` | Boîte `OverlapOnlyPawn` ; au premier passage d'un joueur (serveur) : savoir optionnel, puis `NotifyPlayerEvent(tag, XP)` ; sans tag, l'XP n'est versée qu'à la première obtention du savoir. Découverte **non partagée** au groupe. | serveur |

### 5.3 Règles codées

- **XP et niveaux** : `AddXP` (serveur) ajoute en saturation `int64`, passe autant de niveaux que nécessaire jusqu'à `MaxLevel` (80) ; chaque niveau donne les points de glyphe de sa tranche (4 / 6 / 8) et `AwakeningPointsPerLevel` (1). L'XP au-delà du plafond reste dans `TotalXP`.
- **Déclencheurs** (`GrantTrigger`, une seule fois par joueur) : un tag de boss donne `AwakeningPointsPerBoss` (1) ; chaque glyphe `bHidden` dont `SecretTrigger` correspond est soit **révélé** (`RevealOnly`, réglage par défaut), soit **débloqué gratuitement** (`AutoUnlock`) si ses autres prérequis (niveau, parents, savoirs) sont remplis, sinon révélé.
- **État d'un glyphe** (`ComputeGlyphState`) : `Unlocked` s'il est possédé ; `Hidden` s'il est secret, non révélé et que son déclencheur n'est pas obtenu ; sinon `Available` si niveau, parents, savoirs et déclencheur sont réunis, `Locked` sinon. Les **points** ne changent pas l'état : ils ne sont vérifiés qu'à l'achat (`CanUnlockGlyph`).
- **Achat** : voir la séquence du § 2.8. `Validate` renvoie toujours vrai ; le serveur refuse proprement par `CanUnlockGlyph` / `CanRankUpSkill` (journal `Verbose`, **aucun retour au client**).
- **Passifs** (`RebuildDerivedState`) : somme `ValuePerRank × rang` par `EEMBPassiveStat` ; poussés vers le pawn par `ApplyModifiersToPawn` à chaque changement (côté serveur) et à chaque possession (`AEMBPlayerCharacter::PossessedBy`).

| Statistique | Application dans le code |
|---|---|
| `PhysicalDamage` | `UEMBCombatComponent` : dégâts ×(1 + somme) |
| `AttackStaminaCost` | coût des attaques ×max(0, 1 + somme) (`ScaledAttackCost`) |
| `RiposteMultiplier` | riposte ×(3 + somme) |
| `DodgeIFrames` | fin des i-frames de roulade + somme × 1 frame |
| `GuardStaminaCost` | coût d'endurance en garde ×max(0, 1 + somme) |
| `ParryWindowFrames` | fenêtre de parade parfaite + somme × 1 frame |
| `BuildCost` | `UEMBBuildingComponent::SetBuildCostMultiplier(max(0, 1 + somme))`, borné à [0,1 ; 1], coût arrondi au supérieur, minimum 1 (`GetEffectiveCost`, identique aperçu / serveur) |
| `CarryWeight` | `UEMBInventoryComponent::SetCarryWeightBonus` (charge maximale = 300 + bonus) |
| `HungerThirstRate`, `HarvestYield`, `ComfortRange`, `CorruptionGain`, `FireResistance` | **stockés, non appliqués** (jauges de survie, récolte, confort, corruption et résistances non codées) |
| `None` (compétences actives) | accordent leur `GrantedTag` ; **aucune exécution codée** |

- **Verrou de construction** : `UEMBBuildingComponent::IsPieceUnlocked` lit `RequiredGlyph` de la ligne `DT_BuildPieces` puis `HasGlyph` sur le PlayerState du pawn ; colonne vide = toujours disponible. Sans PlayerState `AEMBPlayerState` (GameMode non configuré), avertissement et **autorisation** (le jeu reste testable).
- **Participants d'un boss** (`NotifyWorldEvent`) : chaque PlayerState du GameState dont le pawn existe (vivant ou mort) à moins de `ParticipantRadius` (10 000 cm) de la source ; source nulle = tous les joueurs. L'XP n'est versée qu'à la première obtention du déclencheur par ce joueur ; un événement monde déjà enregistré n'est jamais redistribué.

### 5.4 Barèmes (référentiel § 7.2, valeurs de `DefaultGame.ini`)

| Paramètre | Valeur |
|---|---|
| XP du niveau L au niveau L+1 | `round(150 × L^1,5)` (`XPBase` 150, `XPExponent` 1,5) |
| Niveau maximal | 80 |
| Points de glyphe par niveau gagné | 4 (niveaux 2-20), 6 (21-50), 8 (51-80) : 496 au niveau 80 |
| Points d'Éveil | 1 par niveau gagné + 1 par boss majeur (une fois chacun) : 82 au maximum |
| XP des boss (par participant) | Gardien 6 000, Séraphe 15 000, Dragon 60 000 |
| Rayon des participants | 10 000 cm |
| `SecretGlyphMode` | `RevealOnly` (**à valider**) |

Ordre de grandeur, calculé à partir de la formule (à vérifier par l'équilibrage) : l'XP cumulée pour atteindre le niveau 80 est de l'ordre de 3,4 millions, alors que les seules sources codées sont le dragon (60 000, une fois) et les volumes de découverte (XP réglée par volume). Voir section 9, P4.

---

## 6. Sauvegarde (codée — v1.2)

Fichiers : `EMBSaveTypes.h`, `EMBSaveGame.h/.cpp`, `EMBSaveSubsystem.h/.cpp`, `EMBGameMode.h/.cpp` ; ajouts dans `EMBStructureSubsystem`, `EMBBuildablePiece`, `EMBInventoryComponent`, `EMBStatsComponent`, `EMBDragonBoss`. Référentiel : § 8 (v1.2).

### 6.1 Principe

- **Qui sauvegarde** : uniquement le serveur (hôte ou dédié), par `UEMBSaveSubsystem` (`UGameInstanceSubsystem`). Les clients n'écrivent rien ; leur personnage est **lié au monde de l'hôte** (pas de personnage portable entre mondes en v1).
- **Fichiers** : 3 mondes (`?Slot=0..2` dans l'URL de la carte, 0 par défaut) × 2 fichiers `EMB_World_<n>_A` et `EMB_World_<n>_B`, écrits **en alternance**. Une écriture interrompue ne peut abîmer que le fichier en cours.
- **Écriture** : instantané complet construit sur le thread de jeu (`BuildSnapshot` : joueurs connectés capturés, pièces, événements monde, champs de contrôle), puis `UGameplayStatics::AsyncSaveGameToSlot`. Une seule écriture en vol ; les demandes reçues pendant ce temps fusionnent en une seule, relancée à la fin (`bSavePending`). À l'arrêt (`EndPlay` du GameMode, `Quit` ou fin de PIE) : `SaveNowBlocking` (`SaveGameToSlot` synchrone), ignorée si une écriture asynchrone est déjà en vol.
- **Déclencheurs** : minuterie de 300 s (`AutosaveIntervalSeconds`, minimum 10 s), événement monde enregistré (boss vaincu), déconnexion d'un joueur (`Logout` → `CapturePlayer`), arrêt du serveur.
- **Report pendant un boss** : sans `bForce`, `RequestSave` est reportée tant qu'un `AEMBDragonBoss::IsEncounterActive()` est vrai (rencontre engagée, boss vivant, au moins un joueur **vivant** à moins de 10 000 cm). La minuterie du point sûr (10 s) relance la sauvegarde reportée dès que la rencontre n'est plus active. Après un wipe, la rencontre n'est plus « active » : la sauvegarde repart.
- **Garde-fous** : aucune écriture avant la fin du chargement ; aucune écriture si un fichier de version inconnue existe ou si des constructions sauvegardées ne peuvent pas être validées (`IsWriteBlocked`) ; un joueur dont l'identité n'a pas été réclamée ou est en double n'est jamais capturé.
- **Dernier point sûr** (toutes les 10 s, hors rencontre de boss) : pawn vivant, au sol, en état de combat `Idle`.

### 6.2 Contenu (`UEMBSaveGame`, propriétés `SaveGame`)

| Bloc | Champs | Source |
|---|---|---|
| En-tête | `SaveFormatVersion` (1), `SaveTimestamp` (UTC, départage A/B), `SlotIndex` | `BuildSnapshot` |
| Monde (`FEMBWorldSaveData`) | `WorldEvents` : événements monde acquis (`Event.Boss.Killed.*`) | `UEMBProgressionSubsystem::GetWorldEvents` |
| Joueurs (`FEMBPlayerSaveData`, clé `StableId`) | `Progression` (`FEMBProgressionSaveData`), `bHasPawnState`, `Resources`, `Health`, `Stamina`, `bHasPawnTransform`, `PawnTransform`, `bHasLastSafeTransform`, `LastSafeTransform` | `ExportSaveData`, `ExportResources`, jauges et transform du pawn ; **toutes** les entrées connues sont conservées, y compris celles des joueurs absents |
| Constructions (`FEMBPieceSaveData`) | `SaveId` (1..N), `RowName`, `PieceClassPath` (informatif), `Transform`, `Health`, `Material`, `Category`, `bAnchoredToGround`, `PlacerStableId`, `SupportLinkIds` | pièces posées par des joueurs et non effondrées (`WasPlacedByPlayer`) ; les pièces placées dans le niveau reviennent avec le niveau |
| Contrôle de fin (**derniers champs déclarés**) | `ExpectedPlayerCount`, `ExpectedPieceCount`, `bComplete` | `FinalizeForWrite` |

**Joueur mort au moment de la capture** (`DeathSaveBehavior`, **à valider**) : PV et endurance enregistrés pleins ; avec `RespawnAtSafePoint` (défaut), position = dernier point sûr, ou PlayerStart s'il n'est pas connu ; avec `KeepPosition`, position telle quelle. Aucune pénalité de mort.

**Non sauvegardé, recalculé** : intégrité des pièces (recalcul global au chargement), `GrantedTags` et passifs (dérivés), état de combat, état d'un combat de boss en cours (seule la défaite du boss est persistée), identifiant de base, `PlacerPlayerId` de session (remis à `INDEX_NONE`).

### 6.3 Chargement et validation

Séquence complète au § 2.10. Règles appliquées à chaque fichier lu :
1. **Bon monde** (`SlotIndex` = monde actif), sinon ignoré.
2. **Version** (`MigrateToCurrent`) : supérieure au binaire, ou inférieure à 1, ou migration impossible → fichier ignoré **et écritures bloquées** (on ne détruit jamais ce que l'on ne sait pas lire).
3. **Complétude** (`IsComplete` : `bComplete` vrai et décomptes égaux au contenu relu) → sinon ignoré, l'autre fichier prend le relais.
4. Le plus récent valide l'emporte (`IsNewerThan` : horodatage, puis version) ; la prochaine écriture vise l'autre fichier.

**Bornes à l'import** : 256 joueurs, 15 000 pièces, transform sans NaN, échelle 1 (± 0,01), coordonnées ≤ 2 097 152 cm ; classe, matériau et catégorie d'une pièce **relus dans `DT_BuildPieces`** (jamais dans le fichier) ; ressources inconnues ignorées, doublons fusionnés, quantités plafonnées ; progression assainie (niveau **déduit de l'XP**, points plafonnés à ce que le niveau et les boss permettent, glyphes et nœuds inconnus ignorés si les tables sont chargées).

**Constructions** : `BeginBulkLoad` (adjacence seule) → création différée de chaque pièce (`InitializePiece` puis `RestoreFromSave(PV, PlacerStableId)`) → `EndBulkLoad` : un recalcul global en O((V+E) log V) depuis toutes les ancres ; les pièces à intégrité ≤ 0 sont **détruites sans effet visible** ; les `SupportLinkIds` servent uniquement de contrôle (écart journalisé, jamais bloquant). Coût : une seule passe synchrone au démarrage du serveur (estimation du code : quelques centaines de ms pour 5 000 pièces, **non mesurée**).

**Joueurs** : `RestorePlayer` (à `PostLogin`) réclame l'identité ; si le chargement n'est pas terminé, la demande attend `FinishLoading`. La progression est importée tout de suite (PlayerState) ; le pawn (inventaire, PV, endurance, position par `TeleportTo` avec repli) est restauré dès qu'il existe, avec nouvelles tentatives toutes les 0,25 s pendant 20 s (`PawnRestoreTimeoutSeconds`).

**Dragon déjà vaincu** : `RestoreWorldEvents` puis `ApplyDefeatedFromSave` sur chaque `AEMBDragonBoss` (le `BeginPlay` du dragon fait le même contrôle, utile si les événements sont déjà restaurés) : dragon caché, sans collision ni IA, `bIsDead` répliqué, ni XP ni `OnBossDefeated`, arène en état final calme. Le dragon n'est **pas détruit** (références de l'arène et des Blueprints conservées).

### 6.4 Format et versionnage

- `UEMBSaveGame::CurrentFormatVersion = 1`. Évolution : incrémenter la constante, ajouter un cas `MigrateStep(N)` (N → N+1), **ne jamais renommer ni réutiliser une propriété** (la sérialisation étiquetée d'Unreal tolère l'ajout de champs, qui prennent leur valeur par défaut).
- Les références aux données sont des **noms de ligne** (`FName`) ; une ligne supprimée est ignorée au chargement. Les énumérations (`EEMBBuildMaterial`, `EEMBPieceCategory`) sont écrites dans le fichier mais **relues dans la table** au chargement ; ne jamais réordonner leurs entrées par prudence.
- La détection d'un fichier tronqué repose sur l'hypothèse que les propriétés sont écrites dans l'ordre de déclaration et qu'une lecture tronquée laisse `bComplete` à faux ; le comportement réel du moteur face à un fichier coupé (objet partiel ou `nullptr`) est **à vérifier par un test** (fichier volontairement tronqué). Dans les deux cas le fichier est écarté.
- Le choix A/B repose sur l'horloge UTC de la machine hôte : un recul d'horloge peut faire préférer l'ancien fichier (risque faible, non traité).

### 6.5 Identifiant stable d'un joueur (`UEMBSaveSubsystem::GetStablePlayerId`)

| Cas | Clé | Limite |
|---|---|---|
| Service en ligne réel (Steam, EOS…) : `GetUniqueId()` valide et de type autre que `NULL` | `GetUniqueId().ToString()` | stable entre sessions et machines |
| Hôte local (listen ou solo) | `LocalHost` (fixe) | le personnage de l'hôte est lié **au monde**, pas au compte : un autre joueur qui héberge ce monde reprend le personnage `LocalHost` |
| Sinon (client avec l'Online Subsystem `NULL` : PIE, LAN) | `Guest_<nom assaini>` (lettres et chiffres) | **non garanti unique** ni stable : changer de nom = personnage neuf ; deux invités de même nom → le second démarre un personnage **neuf non persisté** (détection par `ActiveIds`) |

L'identifiant est calculé une fois par connexion (`ControllerIds`) puis réutilisé. Les journaux n'écrivent qu'un hachage 32 bits (`ShortId`), jamais l'identifiant de plateforme ni le nom. Le poseur d'une pièce est mémorisé par cet identifiant (`PlacerStableId`, serveur seul, non répliqué) **dès la pose** (`UEMBBuildingComponent` appelle `SetPlacerStableId`) ; à la capture, un poseur vide est complété depuis le `PlacerPlayerId` de session si le joueur est encore connecté. Le poseur est donc aussi fiable que l'identifiant : pour un invité, c'est son nom.

---

## 7. Réplication

### 7.1 Tableau des données

| Donnée | Répliquée ? | Condition | Qui fait autorité | Notes |
|---|---|---|---|---|
| Stats : `Health` | Oui (RepNotify) | aucune (tous) | Serveur | `OnRep_Health` → `OnAttributeChanged` |
| Stats : `MaxHealth` | Oui (RepNotify) | aucune | Serveur | change pour la mise à l'échelle coop du boss |
| Stats : `Stamina` | Oui (RepNotify) | `COND_OwnerOnly` | Serveur | HUD du propriétaire uniquement |
| Stats : `Poise` | Non (valeur exacte serveur) ; copie `RepPoise` répliquée (RepNotify) | aucune | Serveur | `RepPoise` n'est rafraîchie que si la poise varie d'au moins 1 point (ou touche 0 / le maximum) : la régénération continue ne génère pas un envoi par tick ; posture visible (barre du boss) |
| Stats : `MaxStamina`, `MaxPoise` | Oui | `COND_InitialOnly` | Serveur | envoyées une fois |
| Stats : délais, multiplicateur de régénération | Non | — | Serveur | Tick de régénération serveur seulement |
| Combat : `CurrentState` | Oui (RepNotify) | aucune | Serveur (FSM) | `OnRep_CurrentState` → `OnCombatStateChanged` |
| Combat : `ComboIndex` | Oui | aucune | Serveur | sert à la prédiction du coup suivant |
| Combat : fenêtres, compteurs i-frames / hyper-armure, buffer, temps | Non | — | Serveur | sur le client, les notifies alimentent des copies locales pour la prédiction |
| Combat : montages | Multicast | attaques, roulade, parade : `Multicast_PlayMontage` **Unreliable** ; garde, charge, étourdissement, rupture de garde, mort : `Multicast_PlayMontageReliable` **Reliable** ; arrêt ciblé `Multicast_StopMontage(montage, fondu)` **Reliable** | Serveur | propriétaire : ignore si montage prédit en cours ; l'arrêt ne vise que la garde ou la charge, jamais une action lancée juste après |
| Combat : résultat de coup, parade réussie | Multicast Unreliable | — | Serveur | `OnHitReceived`, `OnParrySuccess` ; rien pour `bDamageOverTime` |
| Combat : confirmation de coup à l'attaquant | Client RPC Unreliable | propriétaire de l'attaquant | Serveur | `Client_ConfirmHit` → `OnHitConfirmed` |
| Combat : charge portée (`EncumbranceRatio`) | Non | — | calculée sur chaque machine depuis l'inventaire répliqué | le propriétaire prédit la bonne roulade avec la même valeur |
| Inventaire : `Resources` | Oui (RepNotify) | `COND_OwnerOnly` | Serveur | `TArray` de paires (pas de `TMap`) ; poids déduit localement (`GetCurrentWeight`) |
| Construction : mode, pièce choisie, aperçu, fantôme | Non | — | Client local | purement local ; le serveur ne connaît pas le mode construction |
| Pièce : `PieceRowName`, `Material`, `Category`, `bAnchoredToGround`, `PlacerPlayerId` | Oui | `COND_InitialOnly` | Serveur | dans le bunch initial (spawn différé) |
| Pièce : `CurrentHealth`, `Integrity`, `bCollapsed` | Oui (RepNotify) | aucune, mais acteur **dormant** (`DORM_DormantAll`) | Serveur | chaque écriture serveur suivie de `FlushNetDormancy` + `ForceNetUpdate` (intégrité : seulement si la valeur change) |
| Pièce : identifiant de base | Non | — | calculé à la demande (`UEMBStructureSubsystem::GetBaseId`, O(taille de la base)) | non stocké, non répliqué |
| Pièce : mouvement | Non | `SetReplicateMovement(false)` | — | `NetCullDistance` 15 000 cm, `NetUpdateFrequency` 2 |
| Pièce : effets d'effondrement | Multicast Unreliable | — | Serveur | débris lourds jamais sur serveur dédié |
| Graphe de structure | Non | — | Serveur (intégrité) | chaque machine construit son graphe local pour la prédiction d'aperçu |
| Dragon : `CurrentPhase`, `bIsTransitioning`, `bIsStunned`, `bIsDead` | Oui (RepNotify) | aucune | Serveur | `bAlwaysRelevant`, `NetUpdateFrequency` 30 |
| Dragon : `bIsFlying`, `bIsVulnerable` | Oui | aucune | Serveur | — |
| Dragon : PV, posture | via son `UEMBStatsComponent` | voir lignes Stats | Serveur | — |
| Dragon : `StateTags` | Non | — | reconstruits localement depuis les propriétés répliquées | — |
| Dragon : menace, recharges, historique, brûlures, zones de feu | Non | — | Serveur | zones de feu : visuel par `Multicast_SpawnFireZoneVFX` |
| Dragon : montages, souffle (début/fin), arrêt des montages | Multicast **Reliable** (4 RPC : `Multicast_PlayMontage`, `Multicast_StopMontages`, `Multicast_StartBreathVFX`, `Multicast_StopBreathVFX`) | — | Serveur | état visible durable : une perte laisserait un souffle affiché |
| Dragon : VFX ponctuels, VFX des zones de feu, télégraphes, marqueurs au sol (y compris foudre) | Multicast **Unreliable** (4 RPC : `Multicast_SpawnVFXAtLocation`, `Multicast_SpawnFireZoneVFX`, `Multicast_Telegraph`, `Multicast_ShowGroundMarker`) | — | Serveur | ignorés sur serveur dédié ; perte tolérée (voir limites) |
| Contrôleur IA, Blackboard, BT | Non (serveur seul) | — | Serveur | — |
| Arène : décor par phase, lave, tempête de cendres | Non (acteur non répliqué) | — | Déduit de `CurrentPhase` sur chaque machine ; dégâts de lave et foudre par timers **serveur** | rattrapage des phases manquées à la liaison ; marqueurs de foudre diffusés par le boss |
| Personnage : sprint | Dans les **mouvements** (`FLAG_Custom_0` des saved moves de `UEMBCharacterMovementComponent`) | propriétaire → serveur | Serveur pour l'endurance (`ForbidSprint`, `Client_StopSprint`) ; vitesse rejouée à l'identique des deux côtés | plus de RPC `Server_SetSprinting` |
| Personnage : déplacement, saut | `UCharacterMovementComponent` (moteur) | — | Serveur avec prédiction client native | coût du saut facturé serveur (`OnJumped`) |
| Progression : `Level`, `TotalXP` | Oui (RepNotify `OnRep_Level`, `OnRep_TotalXP`) | aucune (tous les clients : UI de groupe) | Serveur | porté par le **PlayerState** (`NetUpdateFrequency` 10 Hz, `ForceNetUpdate` à chaque changement) |
| Progression : `UnspentGlyphPoints`, `UnspentAwakeningPoints`, `UnlockedGlyphs`, `RevealedGlyphs`, `SkillRanks`, `Knowledge`, `ObtainedTriggers` | Oui (RepNotify) | `COND_OwnerOnly` | Serveur | `TArray` et `FGameplayTagContainer` (pas de `TMap`) ; les partenaires ne voient ni glyphes ni compétences |
| Progression : `GrantedTags`, `Modifiers`, états de glyphes en cache | Non | — | recalculés sur chaque machine depuis l'état répliqué | `RebuildDerivedState`, `RefreshGlyphStates` |
| Passifs de combat (`UEMBCombatComponent::PassiveModifiers`) | Non | — | Serveur | appliqués aux calculs serveur (dégâts, coûts, i-frames, parade) ; la prédiction client n'en a pas besoin |
| Construction : `BuildCostMultiplier` | Oui | `COND_OwnerOnly` | Serveur | l'aperçu du client calcule le même coût que le serveur (`GetEffectiveCost`) |
| Inventaire : `CarryWeightBonus` | Oui (RepNotify) | `COND_OwnerOnly` | Serveur | la charge maximale (et donc la roulade prédite) est identique des deux côtés |
| Pièce : `PlacerStableId` | Non | — | Serveur | identifiant stable du poseur, pour la sauvegarde |
| Dragon vaincu d'après la sauvegarde | via `bIsDead` (RepNotify) + visibilité et collision de l'acteur (répliquées par le moteur) | aucune | Serveur | `OnRep_IsDead` diffuse `OnEncounterEnded` : arène calme aussi chez un client qui rejoint |
| Événements monde (`UEMBProgressionSubsystem::WorldEvents`) | **Non** | — | Serveur seul | `IsWorldEventRecorded` renvoie faux sur un client ; un client déduit un boss vaincu de l'état répliqué de l'acteur |
| Sauvegarde (`UEMBSaveSubsystem`) | Non | — | Serveur seul | aucune donnée de sauvegarde ne transite par le réseau |

### 7.2 RPC existants

| RPC | Classe | Type | Validation | Rôle |
|---|---|---|---|---|
| `Server_HandleInput(EEMBCombatInput)` | `UEMBCombatComponent` | Server, Reliable | `WithValidation` : valeur d'énumération dans la plage | entrée de combat |
| `Client_RejectPrediction()` | `UEMBCombatComponent` | Client, Reliable | — | coupe le montage prédit refusé |
| `Multicast_PlayMontage(UAnimMontage*, float, FName)` | `UEMBCombatComponent` | NetMulticast, Unreliable | — | animation d'action (attaques, roulade, parade) |
| `Multicast_PlayMontageReliable(UAnimMontage*, float, FName)` — **nouveau v1.1** | `UEMBCombatComponent` | NetMulticast, Reliable | — | montages dont la perte se voit : garde, charge, étourdissement, rupture de garde, mort |
| `Multicast_StopMontage(UAnimMontage*, float)` | `UEMBCombatComponent` | NetMulticast, Reliable | — | arrêt **ciblé** de la garde ou de la charge |
| `Client_ConfirmHit(AActor*, EEMBHitResult, FVector)` — **nouveau v1.1** | `UEMBCombatComponent` | Client, Unreliable | — | retour de coup à l'attaquant (`OnHitConfirmed`) |
| `Multicast_BroadcastHitReceived(...)` | `UEMBCombatComponent` | NetMulticast, Unreliable | — | retour de coup reçu (jamais pour `bDamageOverTime`) |
| `Multicast_ParrySuccess(AActor*)` | `UEMBCombatComponent` | NetMulticast, Unreliable | — | parade réussie |
| `Client_StopSprint()` | `AEMBPlayerCharacter` | Client, Reliable | — | refus ou épuisement (le souhait de sprint, lui, voyage dans les mouvements ; `Server_SetSprinting` a été **supprimé**) |
| `Server_PlacePiece(FName, FTransform)` | `UEMBBuildingComponent` | Server, Reliable | `WithValidation` : NaN, nom vide, hors monde | demande de pose |
| `Client_PlacementResult(EEMBPlacementResult, float)` | `UEMBBuildingComponent` | Client, Reliable | — | réponse du serveur (dont `LimitReached`) |
| `Multicast_PlayCollapseEffects(bool)` | `AEMBBuildablePiece` | NetMulticast, Unreliable | — | effets d'effondrement |
| `Multicast_PlayMontage`, `Multicast_StopMontages`, `Multicast_StartBreathVFX`, `Multicast_StopBreathVFX` | `AEMBDragonBoss` | NetMulticast, Reliable | — | montages et souffle du boss |
| `Multicast_SpawnVFXAtLocation`, `Multicast_SpawnFireZoneVFX` (**nouveau v1.1**, une liste de positions par traînée), `Multicast_Telegraph`, `Multicast_ShowGroundMarker` | `AEMBDragonBoss` | NetMulticast, **Unreliable** | — | mise en scène ponctuelle du boss et marqueurs de foudre de l'arène |
| `Server_UnlockGlyph(FName)` — **nouveau v1.2** | `UEMBProgressionComponent` (sur le PlayerState, appartenant à la connexion du joueur) | Server, Reliable | `WithValidation` : **toujours vrai** (identifiant inconnu = refus propre, pas une déconnexion) | achat d'un glyphe ; le serveur refait `CanUnlockGlyph` ; **aucune réponse en cas de refus** |
| `Server_RankUpSkill(FName)` — **nouveau v1.2** | `UEMBProgressionComponent` | Server, Reliable | idem | un rang de compétence de plus ; le serveur refait `CanRankUpSkill` ; aucune réponse en cas de refus |

Les RPC de progression ne sont pas limités en fréquence (contrairement à `Server_PlacePiece`, 0,1 s, et à `Server_HandleInput`, 30/s) : chaque appel parcourt au plus la table des glyphes au recalcul ; à surveiller si un client malveillant inonde le serveur (point P7, section 9).

### 7.3 Ce qui est prédit côté client

- **Montages de combat** (`PredictLocally`) : attaque légère (y compris le coup suivant dans la fenêtre de combo), lourde, début de charge, roulade, parade — uniquement si l'état répliqué est `Idle`, `Block` ou `Recovery` avec fenêtre de combo ouverte, et si l'endurance répliquée est > 0. Pas de prédiction pour la garde ni le relâchement de charge.
- **Sprint** : vitesse prédite par `UEMBCharacterMovementComponent::GetMaxSpeed` et rejouée par le serveur à partir des saved moves (plus de correction de position au démarrage ni à l'arrêt).
- **Roulade selon la charge portée** : le propriétaire calcule le même `EncumbranceRatio` que le serveur depuis l'inventaire répliqué (montage léger / moyen / lourd, roulade refusée au-delà de 100 %).
- **Aperçu de construction** : placement, accrochage, couleur et intégrité prédite (graphe local + intégrité répliquée des voisins), coût effectif (multiplicateur répliqué) et verrou de glyphe (`IsPieceUnlocked` sur l'état de progression répliqué).
- **Arbre de glyphes** : l'UI lit `GetGlyphState` / `CanUnlockGlyph` localement ; l'achat lui-même n'est **pas** prédit (l'état change au retour de la réplication).
- **Déplacement et saut** : prédiction native du `CharacterMovementComponent`.
- **Jamais prédits** : dégâts, jauges, inventaire, état de combat, effets d'un coup, pose effective d'une pièce, tout ce qui concerne le boss.

### 7.4 Limites connues (documentées dans le code ou constatées à la lecture)

1. **Pas de rollback.** Un montage prédit refusé est coupé (`Client_RejectPrediction`) ; en cas de désaccord, l'état répliqué fait foi et seul l'habillage visuel est transitoirement faux.
2. **Fenêtre de parade mesurée en temps serveur** : un joueur à 100 ms de ping perçoit les 10 frames raccourcies. `ParryLatencyTolerance` existe mais vaut 0 (valeur du référentiel). Lié à Q6 de `03_Scalabilite.md`.
3. **Pas de compensation de latence pour la détection des coups** : le balayage de la lame utilise les positions serveur ; ce que voit le client attaquant peut différer.
4. ~~Sprint sans « saved moves »~~ — **corrigé v1.1** : `UEMBCharacterMovementComponent` transporte le souhait de sprint dans les drapeaux compressés ; l'endurance reste facturée par un timer serveur (0,2 s).
5. **Montages d'action courts en Unreliable** (attaques, roulade, parade) : un paquet perdu = une animation manquante chez un autre client (l'état de gameplay reste juste). Les montages durables (garde, charge, étourdissement, mort) sont désormais Reliable.
6. **Animations évaluées sur serveur dédié** (`AlwaysTickPoseAndRefreshBones` sur le joueur et le dragon) : nécessaire aux notifies de fenêtre de coup et aux sockets, coût à mesurer. Les i-frames de roulade et la fenêtre de parade n'en dépendent plus (timers).
7. **Multicasts du dragon** : réduits à 4 Reliable (montages, souffle) ; les 4 cosmétiques ponctuels sont Unreliable. Contrepartie : un `Multicast_SpawnFireZoneVFX` perdu laisse des zones de feu **invisibles mais dangereuses** pour ce client (voir I11).
8. **Push Model non utilisé** : `03_Scalabilite.md` (§ 13.1) le retient, mais aucune propriété n'est marquée sale explicitement dans le code et `net.IsPushModelEnabled` n'est pas réglé dans `Config/DefaultEngine.ini`. **Ouvert.**
9. **Un acteur répliqué par pièce** (dormant) : **retenu pour la v1 par décision utilisateur** ; l'acteur « Base » à Fast Array (`03_Scalabilite.md` D5, Q4) reste une évolution. Le format de sauvegarde des pièces (`FEMBPieceSaveData`, une entrée par pièce, sans regroupement par base) est indépendant de ce choix : une migration vers le Fast Array ne demanderait qu'un changement du chargement (`SpawnSavedPieces`), pas du fichier. La dépendance `NetCore` reste déclarée.
10. **Retour de coup asymétrique** : l'attaquant reçoit `OnHitConfirmed` pour tous ses coups ; les **autres** joueurs ne voient un coup porté au dragon ou à une pièce qu'à travers la variation de PV répliquée (pas de multicast de résultat côté dragon ni pièce).
11. **PlayerState à 10 Hz** : le PlayerState est relevé de 1 Hz (défaut) à 10 Hz pour que les achats et niveaux se voient vite ; coût faible en coop 1-4, à garder en tête si des données volumineuses y sont ajoutées.

---

## 8. Arborescence et dépendances

### 8.1 Fichiers du module (`Source/Emberfall/`)

```
EmberfallGDD/
├── Emberfall.uproject                      UE 5.4, module Runtime « Emberfall », plugins EnhancedInput et Niagara
├── README.md                               installation, réglages d'assets (collisions, Physics Assets)
├── Config/
│   ├── DefaultEngine.ini                   Enhanced Input ; canaux EMBMeleeHit (GameTraceChannel1, Ignore par défaut)
│   │                                       et EMBBuilding (GameTraceChannel2, Block par défaut)
│   │                                       GlobalDefaultGameMode = EMBGameMode
│   ├── DefaultGame.ini                     nom du projet ; [EMBProgressionSettings] tables, règles, barèmes ;
│   │                                       [EMBSaveSubsystem] BuildPiecesTable ; cuisson de /Game/Data
│   └── Tags/EmberfallGlyphs.ini            tags Glyph.* (colonne GrantedTag de DT_Glyphs)
│       Tags/EmberfallSkills.ini            tags Skill.* (colonne GrantedTag de DT_SkillNodes)
├── Data/
│   ├── DT_BuildPieces.csv                  31 lignes (FEMBBuildPieceRow, colonne RequiredGlyph v1.2)
│   ├── DT_DragonAttacks.csv                11 lignes (FEMBDragonAttackRow, colonne StaminaDamage v1.1)
│   ├── DT_Glyphs.csv                       36 lignes (FEMBGlyphRow)
│   └── DT_SkillNodes.csv                   20 lignes (FEMBSkillNodeRow, colonnes PassiveStat, ValuePerRank,
│                                           RequiredKnowledge v1.2)
├── Source/
│   ├── Emberfall.Target.cs                 cible Game (BuildSettingsVersion.V5, ordre d'includes 5.4)
│   ├── EmberfallEditor.Target.cs           cible Editor
│   ├── EmberfallServer.Target.cs           cible Server (serveur dédié)
│   └── Emberfall/                          module (détail ci-dessous)
└── docs/                                   00 référentiel, 01 GDD, 02 architecture, 03 scalabilité

Source/Emberfall/
├── Emberfall.Build.cs
├── Public/
│   ├── Emberfall.h                         module + catégorie de log LogEmberfall
│   ├── EMBGameplayTags.h                   EMBTags (tags natifs)
│   ├── EMBCombatTypes.h                    énums de combat, FEMBHitData, FEMBHitWindowParams, FEMBAttackDefinition, EMBCombatDefaults
│   ├── EMBHitReceiver.h                    interface IEMBHitReceiver
│   ├── EMBStatsComponent.h                 jauges (joueur et boss)
│   ├── EMBCombatComponent.h                FSM de combat, détection et résolution des coups
│   ├── EMBAnimNotifyState_HitWindow.h      fenêtre de coup
│   ├── EMBAnimNotifyState_ComboWindow.h    fenêtre de combo
│   ├── EMBAnimNotifyState_IFrames.h        invulnérabilité / hyper-armure
│   ├── EMBPlayerCharacter.h                personnage jouable
│   ├── EMBCharacterMovementComponent.h     NOUVEAU v1.1 : sprint prédit (FEMBSavedMove, données de prédiction)
│   ├── EMBProgressionTypes.h               énums (passifs, modes, états), FEMBPassiveModifiers, FEMBSkillRank,
│   │                                       FEMBProgressionSaveData, lignes glyphes / compétences
│   ├── EMBProgressionSettings.h            NOUVEAU v1.2 : réglages (UDeveloperSettings, DefaultGame.ini)
│   ├── EMBProgressionComponent.h           NOUVEAU v1.2 : état de progression d'un joueur (sur le PlayerState)
│   ├── EMBProgressionSubsystem.h           NOUVEAU v1.2 : tables, événements monde, distribution
│   ├── EMBPlayerState.h                    NOUVEAU v1.2 : PlayerState (porte la progression)
│   ├── EMBGameMode.h                       NOUVEAU v1.2 : GameMode (cycle de vie de la sauvegarde)
│   ├── EMBDiscoveryVolume.h                NOUVEAU v1.2 : volume de lieu découvert
│   ├── EMBSaveTypes.h                      NOUVEAU v1.2 : FEMBPieceSaveData, FEMBPlayerSaveData, FEMBWorldSaveData
│   ├── EMBSaveGame.h                       NOUVEAU v1.2 : UEMBSaveGame (version, contrôle de fin, migration)
│   ├── EMBSaveSubsystem.h                  NOUVEAU v1.2 : sauvegarde (UGameInstanceSubsystem, serveur)
│   ├── EMBInventoryComponent.h             inventaire de ressources, poids porté, bonus de charge, export/import
│   ├── EMBBuildingTypes.h                  énums (dont LimitReached), EMBBuildingDefaults, FEMBBuildPieceRow, FEMBPlacementEvaluation
│   ├── EMBBuildingComponent.h              mode construction, aperçu, RPC de pose, limites
│   ├── EMBBuildablePiece.h                 pièce posée (répliquée, dormante, IEMBHitReceiver)
│   ├── EMBStructureSubsystem.h             graphe d'intégrité, sous-graphe dépendant, effondrement par niveaux
│   ├── EMBDragonTypes.h                    FEMBDragonAttackRow, EMBDragonDefaults
│   ├── EMBDragonBoss.h                     boss Vaelthyrax
│   ├── EMBDragonAIController.h             contrôleur IA + schéma du BT
│   ├── EMBDragonArena.h                    arène évolutive (lave, foudre et tempête de phase 3)
│   ├── BTTask_EMBSelectAttack.h
│   ├── BTTask_EMBDragonBreath.h
│   ├── BTTask_EMBDragonDiveAttack.h
│   ├── BTService_EMBUpdateCombatContext.h
│   └── BTDecorator_EMBDragonPhase.h
└── Private/                                29 .cpp : un par en-tête (dont Emberfall.cpp, EMBGameplayTags.cpp) ;
                                            EMBCombatTypes, EMBHitReceiver, EMBBuildingTypes, EMBDragonTypes,
                                            EMBProgressionTypes, EMBSaveTypes : en-têtes seuls
```

Des copies de sauvegarde `*.bak-20261023*` des fichiers modifiés coexistent dans `Public/`, `Private/`, `Data/` et `Config/` ; elles ne sont pas compilées (extension) mais sont à exclure du dépôt (`.gitignore`) ou à archiver après validation. Un fichier `Data/DT_SkillNodes.csv.tmp` est aussi présent : à vérifier (reliquat probable), **non supprimé**.

Fichiers **prévus, non codés** : `EMBGameState.h/.cpp` (état de monde répliqué), `EMBPlayerController.h/.cpp` (HUD, menus), widgets UMG (`Public/UI/` si l'on sort de l'organisation « à plat » imposée par le référentiel § 1 — à arbitrer).

### 8.2 Dépendances de modules (`Emberfall.Build.cs`, état réel)

`PCHUsage = UseExplicitOrSharedPCHs`.

| Module | Public / Privé | Utilisé par (includes constatés) |
|---|---|---|
| `Core`, `CoreUObject`, `Engine` | Public | tout le module |
| `InputCore`, `EnhancedInput` | Public | `AEMBPlayerCharacter` (plugin `EnhancedInput` activé dans le `.uproject`) |
| `GameplayTags` | Public | `EMBTags`, `FEMBHitData`, dragon, progression, sauvegarde |
| `DeveloperSettings` | Public | `UEMBProgressionSettings` (**ajouté v1.2**) |
| `AIModule` | Public | contrôleur, Blackboard, nœuds BT, perception |
| `NavigationSystem` | Public | aucun include direct ; utile au `MoveTo` du BT et au navmesh |
| `Niagara` | Public | effets du dragon et des pièces |
| `UMG` | Public | **aucun usage** (UI prévue) |
| `NetCore` | Public | **aucun usage direct** (`FFastArraySerializer` pour l'évolution D5) |
| `GameplayTasks` | Privé | requis par les nœuds BT (tâches) |
| `Slate`, `SlateCore` | Privé | **aucun usage** |

`Chaos`, `GeometryCollectionEngine` et `PhysicsCore` ont été **retirés** (aucun include ; les débris passent par un acteur Blueprint). Recommandation inchangée : conserver UMG et NetCore (prévus), retirer `Slate`/`SlateCore` tant qu'ils ne sont pas inclus — la compilation tranchera (le code n'a jamais été compilé).

### 8.3 Dépendances internes (qui inclut qui)

```
EMBCombatTypes ◄── EMBHitReceiver ◄── EMBCombatComponent ◄── EMBPlayerCharacter ──► EMBCharacterMovementComponent
       ▲                ▲                 ▲      ▲                  └──► EMBInventoryComponent (poids)
       │                │                 │      └── EMBAnimNotifyState_* (FindFromMesh)
EMBDragonTypes ◄── EMBDragonBoss ─────────┘ (ApplyHitToActor)
                        ▲
   EMBDragonAIController, BTTask_*, BTService_*, BTDecorator_*, EMBDragonArena
EMBBuildingTypes ◄── EMBBuildablePiece (implémente IEMBHitReceiver) ◄── EMBStructureSubsystem
                 ◄── EMBBuildingComponent ──► EMBInventoryComponent
                                          ──► EMBCombatComponent (lecture de l'état Dead, serveur)
EMBStatsComponent ◄── combat, personnage, dragon, arène
EMBProgressionTypes ◄── EMBCombatComponent (FEMBPassiveModifiers)
                    ◄── EMBProgressionComponent ◄── EMBPlayerState ◄── EMBGameMode
                    ◄── EMBProgressionSubsystem ◄── EMBDragonBoss.cpp, EMBDiscoveryVolume.cpp
EMBProgressionComponent ◄── EMBBuildingComponent.cpp (HasGlyph), EMBPlayerCharacter.cpp (PossessedBy)
EMBSaveTypes (inclut EMBBuildingTypes, EMBInventoryComponent, EMBProgressionTypes) ◄── EMBSaveGame ◄── EMBSaveSubsystem
EMBSaveSubsystem.cpp ──► structure, pièces, inventaire, jauges, combat, dragon, progression (tous les domaines)
EMBBuildingComponent.cpp ──► EMBSaveSubsystem (GetStablePlayerId à la pose)
```

Pas de dépendance circulaire **entre en-têtes**. Liens nouveaux de la v1.2 : le **combat inclut les types de progression** (`FEMBPassiveModifiers`) ; la **construction lit la progression** (`IsPieceUnlocked`) et appelle la sauvegarde (`GetStablePlayerId`) dans son `.cpp` ; le **dragon appelle la progression** (`NotifyWorldEvent`, `IsWorldEventRecorded`) ; la **sauvegarde dépend de tous les domaines** (c'est son rôle, elle n'est incluse par aucun en-tête de jeu). Liens antérieurs à connaître : la **construction dépend du combat** (`EMBBuildablePiece` inclut `EMBHitReceiver.h` donc `EMBCombatTypes.h` ; `UEMBBuildingComponent` lit l'état `Dead` du `UEMBCombatComponent` par `FindComponentByClass`), et le **personnage relie inventaire et combat** (`UpdateEncumbrance`). Le combat, lui, ne connaît toujours ni la construction ni le dragon (il ne voit que l'interface `IEMBHitReceiver`).

---

## 9. Écarts code ↔ référentiel et points de revue

Statut établi le 23/10/2026 par relecture du code (non compilé, non exécuté) : **Corrigé** = le code fait désormais ce que demande le référentiel (v1.2) ou la piste proposée ; **Partiel** = corrigé avec une réserve ; **Ouvert** = rien n'a changé ou le manque persiste. **Rien n'a été compilé ni testé** : aucun statut ci-dessous ne vaut validation fonctionnelle.

### 9.1 Écarts entre le code et `00_Constantes_et_Contrats.md`

| # | Référentiel | Constat v1 | État v1.1 (code actuel) | Statut |
|---|---|---|---|---|
| E1 | § 4.3 : fenêtre de riposte de 1,5 s, premier coup ×3 | `IsRiposteWindowOpen()` jamais appelé | `SweepBlade` multiplie par `CriticalRiposteMultiplier` le premier coup porté à l'attaquant paré (`RiposteTarget`) pendant 1,5 s ; la fenêtre se referme sur `Hit` ou `Killed`. Réserve : voir N1 (cumul avec le critique du boss étourdi). | **Corrigé** |
| E2 | § 4.3 : `CriticalRiposteMultiplier` ×3, `BossPostureBreakDuration` 4 s | constantes inutilisées, doublons dans `EMBDragonDefaults` | `CriticalRiposteMultiplier` est utilisé (riposte). `BossPostureBreakDuration` reste **inutilisé** ; le dragon garde ses doublons `EMBDragonDefaults::CriticalHitMultiplier` et `PostureBreakStunDuration` (mêmes valeurs). | **Ouvert** (faible : risque de divergence) |
| E3 | § 4.2 (v1.1) : garde physique 85 %, feu/givre/foudre 50 %, corruption 30 % | même réduction pour tous les types | `GetBlockReduction(DamageType)` applique les trois taux (`BlockReductionPhysical / Elemental / Corruption`). | **Corrigé** |
| E4 | § 4.2 / § 4.3 (v1.1) : roulade selon la charge, poids unitaires, maximum 300 | `SetEncumbranceRatio` jamais appelé, pas de poids | `UEMBInventoryComponent::GetCurrentWeight` / `GetUnitWeight` / `MaxCarryWeight` = 300 ; le personnage recalcule le ratio à chaque `OnInventoryChanged` (serveur et propriétaire) → `SetEncumbranceRatio` ; i-frames 13/11/9, coût 22 au-delà de 70 %, roulade refusée au-delà de 100 %. | **Corrigé** |
| E5 | § 2 : `EMBBuildablePiece` acteur répliqué par pièce | `03_Scalabilite.md` D5 propose un acteur « Base » à Fast Array | Acteur par pièce ; le graphe est découplé des acteurs (3 points de branchement documentés dans `EMBStructureSubsystem.h`). **Décision utilisateur v1.2 : acteur par pièce pour la v1**, Fast Array en évolution. | **Tranché** (conforme au référentiel) |
| E6 | § 6 : arène P3 — tempête de cendres et foudre (impact 150, télégraphe 1,5 s, rayon 250, toutes les 3 s, décalage 0-400 cm) | non codé | `AEMBDragonArena::StartLightning / StrikeLightning / ResolveLightningImpact` (timers serveur, constantes `EMBDragonDefaults::Lightning*`), marqueur et flash via `Multicast_ShowGroundMarker` ; tempête = événement Blueprint `OnAshStormChanged` (visuel seul). Réserves : N2, N3. | **Corrigé** |
| E7 | § 1 : dossiers « à plat » | respecté | Toujours respecté (les 9 fichiers v1.2 sont ajoutés à plat). À revoir si l'UI est ajoutée. | — |
| E8 | § 3.4 / § 3.6 / § 7.4 : contrats | respectés | Toujours respectés ; ajouts compatibles (`OnHitConfirmed`, `SetEncumbranceRatio`, `GetCurrentWeight`, `PlacerPlayerId` en paramètre par défaut d'`InitializePiece`, `LimitReached` ajouté **en fin** d'énumération). v1.2 : les signatures du § 7.4 sont présentes telles quelles (`SetPassiveModifiers`, `SetBuildCostMultiplier`, `SetCarryWeightBonus`, `ExportResources` / `ImportResources`, `RestoreFromSave`, API du composant et du subsystem de progression) ; colonnes de `FEMBSkillNodeRow` ajoutées **en fin** de ligne. | Conforme |
| E9 | § 3.2 (v1.1) : `bDamageOverTime` | — | Champ présent dans `FEMBHitData` ; posé par la lave, la brûlure et les zones de feu ; **pas** par la foudre (coup normal, conforme). | **Conforme** |
| E10 | § 4.3 (v1.1) : canaux `EMBMeleeHit` / `EMBBuilding` | — | Déclarés dans `Config/DefaultEngine.ini` ; valeurs par défaut de `HitTraceChannel` (GameTraceChannel1) et `PlacementOverlapChannel` (GameTraceChannel2). Les réponses des assets (Physics Assets en Overlap, capsules en Ignore, décor en Block) restent à régler dans l'éditeur (README). | **Conforme** côté code |
| E11 | § 5 (v1.1) : pas de grille par catégorie, limites 3 000/base et 15 000/monde, `GroundTraceDepth` 180 | — | `GetGridStep` (300/150/25), contrôle serveur ±5 cm ou socket ; `CountPiecesInTouchedBases` (parcours borné) et `LimitReached` ; `GroundTraceDepth` = 180. Les limites 10 000 (listen) / 6 000 (Series S) de `03_Scalabilite.md` § 14.2 ne sont **pas** distinguées (une seule valeur, réglable en Blueprint). | **Conforme** (réserve plateforme) |
| E12 | § 6 (v1.1) : vulnérable ×1,5, `StaminaDamage` 10 par tick de souffle, mise à l'échelle figée au début de la rencontre | — | `VulnerableDamageMultiplier` = 1,5 ; colonne `StaminaDamage` dans `DT_DragonAttacks` reprise par `MakeHitData` ; `BeginEncounter` idempotent appelé à la perception, au choix de cible du service et **avant tout dégât** dans `ReceiveEMBHit`. | **Conforme** |
| E13 | § 8 (v1.2) : au chargement, « les pièces à intégrité ≤ 0 s'effondrent normalement » | — | `EndBulkLoad` les **détruit sans effet ni vague visible** (choix documenté dans `EMBStructureSubsystem.h` : personne ne regarde la scène au démarrage). Le code est plus sûr que le texte. | **Écart documentaire** : aligner le référentiel sur le code |
| E14 | § 7.1 (v1.2) : subsystem de progression « actif sur le serveur » | — | Créé sur serveur **et** clients (lecture des tables par l'UI et `IsPieceUnlocked`) ; distribution et `WorldEvents` serveur seul. | Conforme en substance (préciser le texte) |
| E15 | § 7.3 (v1.2) : passifs poussés « à chaque changement et à chaque possession » | — | `RefreshAll` → `PushModifiersToCurrentPawn` (serveur) et `AEMBPlayerCharacter::PossessedBy` → `ApplyModifiersToPawn`. | **Conforme** |
| E16 | § 8 (v1.2) : constructions sauvegardées avec « classe (soft path) » | — | `PieceClassPath` est écrit mais **ignoré au chargement** : classe, matériau et catégorie sont relus dans `DT_BuildPieces` (un fichier modifié ne peut pas charger une classe arbitraire). | Conforme (renforcé) |
| E17 | § 7.2 (v1.2) : XP Gardien 6 000, Séraphe 15 000 | — | Valeurs présentes dans `UEMBProgressionSettings` mais **aucun émetteur** (boss non codés) : voir P3. | **Ouvert** (hors périmètre code actuel) |

### 9.2 Incohérences entre composants repérées à la lecture

| # | Constat v1 | État v1.2 (code actuel) | Statut |
|---|---|---|---|
| I1 | Rien ne pouvait endommager une pièce | `AEMBBuildablePiece` implémente `IEMBHitReceiver` (multiplicateurs par type et matériau) et relie `TakeDamage` à `ApplyStructuralDamage`. Le dragon ne vise toujours que les pawns, ce qui est **conforme** au référentiel v1.1 (arènes en `Building.NoBuildZone`). Réserve : la lame du joueur ne touchera une pièce que si son mesh **bloque** `EMBMeleeHit` ; or la pièce utilise le profil `BlockAll` et le canal est en `Ignore` par défaut — à vérifier dans l'éditeur (voir N4). | **Partiel** |
| I2 | Lame sur `ECC_Pawn` : capsule touchée, points faibles jamais déclenchés | Balayage sur `EMBMeleeHit` (GameTraceChannel1) ; pour chaque acteur, l'impact qui porte un os est préféré. Fonctionnera dès que les assets seront réglés (Physics Asset du dragon en Overlap, capsule en Ignore). | **Corrigé** côté code (réglage d'assets à faire) |
| I3 | Dégâts continus esquivables, counter-hit et multicast en rafale | Drapeau `bDamageOverTime` : dégâts directs dans `ReceiveHit` (ni esquive, garde, parade, counter-hit ni multicast) ; pas de poise dans le chemin « simples jauges ». | **Corrigé** |
| I4 | Posture de parade appliquée directement aux jauges du boss | `ApplyParryPosture` envoie un coup de posture pur (0 dégât, 35 de posture) via `ReceiveEMBHit` quand l'attaquant implémente `IEMBHitReceiver` : invulnérabilité de transition et règle « pas de posture pendant l'étourdissement » respectées. | **Corrigé** |
| I5 | `SetWeaponMesh` jamais appelé | Toujours aucun appelant : la détection repose sur les sockets `weapon_base` / `weapon_tip` du squelette du personnage. | **Ouvert** (équipement non codé) |
| I6 | Aucun retour de coup sur le dragon | `Client_ConfirmHit` → `OnHitConfirmed` sur le client de l'attaquant (pour le dragon, les pièces et les créatures). Les autres joueurs n'ont toujours que la variation de PV (limite 7.4-10). | **Partiel** |
| I7 | Mise à l'échelle à la première détection | Devenu la règle du référentiel v1.1 (« figée au début de la rencontre ») ; `BeginEncounter` est en plus appelé avant tout dégât. | **Corrigé** (par arbitrage) |
| I8 | Pièces sans base ni propriétaire | **v1.2 : identifiant persistant du poseur** `PlacerStableId` (serveur seul), renseigné dès la pose par `GetStablePlayerId`, sauvegardé et restauré (`RestoreFromSave`) ; `PlacerPlayerId` de session reste répliqué (`COND_InitialOnly`) et vaut `INDEX_NONE` après un chargement. Réserves restantes : l'identifiant n'est stable qu'avec un vrai service en ligne (invités identifiés par leur **nom**, hôte = `LocalHost`, § 6.5) ; l'identifiant de base (`GetBaseId`) reste non persistant et change quand la base change ; aucune limite « bases par joueur » ; `Owner` toujours nul ; aucun système ne **lit** encore le poseur (droits, tir ami). | **Partiel** (sauvegarde : corrigé) |
| I9 | `OnBossDefeated` sans abonné | **v1.2** : la mort du dragon appelle directement `UEMBProgressionSubsystem::NotifyWorldEvent` (XP 60 000 et déclencheur aux participants, enregistrement de l'événement, autosave). `OnBossDefeated` reste diffusé, sans abonné C++ (mise en scène Blueprint possible). | **Corrigé** |
| I10 | Réglage de la posture du dragon par réflexion | Affectation directe dans `ConfigureStatsDefaults`. | **Corrigé** |
| I11 | Zones de feu invisibles chez les clients | `Multicast_SpawnFireZoneVFX` (Unreliable) diffusé une fois par traînée, au nettoyage de la tâche. Réserves : visuel absent pendant la traînée (jusqu'à 2,5 s alors que la zone fait déjà mal) ; un paquet perdu = zones invisibles mais dangereuses ; client arrivé en cours de combat : rien ; `SpawnSystemAtLocation` sans pool (contraire à `03_Scalabilite.md` D8). | **Partiel** |
| I12 | Le serveur ignore le mode construction | Inchangé : seul le client filtre les entrées de combat. Le serveur refuse en revanche toute pose à un joueur mort. | **Ouvert** (acceptable) |
| I13 | — Réapparition | **v1.2** : `AEMBGameMode` existe, mais ne gère que la sauvegarde. Un joueur mort reste en `Dead`, mouvement et entrées désactivés, sans réapparition ni remise à zéro des jauges. Seul effet de bord : à la déconnexion, un joueur mort est enregistré PV pleins au dernier point sûr (`DeathSaveBehavior`) ; **se déconnecter puis se reconnecter le ressuscite** (voir P6). | **Ouvert** (mort et réapparition non codées) |
| I14 | — Progression | **v1.2** : composant, subsystem, réglages, PlayerState, volumes de découverte, CSV `DT_SkillNodes`, verrou `RequiredGlyph` (`IsPieceUnlocked` consulte `HasGlyph`), six passifs de combat, le coût de construction et la charge appliqués. Réserves : P1 à P4, P7 (section 9.4). | **Corrigé** (avec réserves) |
| I15 | — Push Model (limite 7.4-8) | Non utilisé (ni marquage explicite, ni réglage `.ini`). | **Ouvert** |

### 9.3 Nouveaux points relevés à la relecture v1.1

| # | Constat | Conséquence | Piste | Gravité |
|---|---|---|---|---|
| N1 | Les critiques se **cumulent** : riposte ×3 (côté joueur, `SweepBlade`) puis ×3 si le dragon est étourdi (`ReceiveEMBHit`), puis point faible (jusqu'à ×1,3) et vulnérabilité ×1,5. Une parade qui brise la posture du dragon suivie d'un coup dans les 1,5 s donne ×9 (jusqu'à ×17,6). | Burst très supérieur à l'intention du référentiel (§ 4.3 décrit **un** coup critique ×3, « riposte sur boss étourdi **ou** premier coup sur l'attaquant paré »). | Ne pas appliquer la riposte si la cible est un `IEMBHitReceiver` qui gère son propre critique, ou plafonner à ×3 ; trancher au référentiel si la riposte s'applique aux boss. | Moyenne |
| N2 | La foudre (`ResolveLightningImpact`) a pour instigateur l'**arène** : le test « de face » de la garde vise le pivot de l'arène, pas le point d'impact. | Garde efficace ou non selon l'orientation par rapport au centre de l'arène, pas à l'éclair. | Instigateur nul (le test retombe sur `HitDirection`) ou `bUnblockable` selon l'intention du GDD. | Faible |
| N3 | `LightningImpactTimers` reçoit un handle tous les 3 s et n'est vidé qu'à `EndPlay`. Le marqueur de foudre est Unreliable : un paquet perdu = éclair sans télégraphe (150 dégâts). | Croissance mémoire lente pendant la phase 3 ; rares coups « injustes ». | Retirer les handles expirés ; marqueur à rendre fiable ou dupliqué si les tests réseau montrent des pertes. | Faible |
| N4 | Collision des pièces : profil `BlockAll` + canal `EMBMeleeHit` en `Ignore` par défaut. Si le réglage est corrigé, rien n'empêche un joueur d'abîmer sa propre base ou celle d'un coéquipier (pas de filtre dans `SweepBlade`). | Soit les coups ne touchent jamais les pièces, soit tir ami sur les constructions. | Régler la réponse du canal sur la classe de pièce et trancher au GDD (dégâts aux bases alliées ?). | Moyenne |
| N5 | La roulade pendant la charge (`ChargedAttack → Dodge`) n'est pas prédite côté client. | Roulade perçue avec un retard d'un aller-retour réseau dans ce seul cas. | Étendre `PredictLocally` à l'état `ChargedAttack` en charge. | Faible |
| N6 | `UEMBBuildingComponent` dépend désormais du combat (lecture de l'état `Dead`) et `AEMBBuildablePiece` de l'interface de combat. | Couplage entre les domaines de Dev A et Dev B (§ 8.3) ; acceptable, mais toute évolution de `EMBCombatTypes.h` touche la construction. | Documenter dans le référentiel (attribution des fichiers) ou passer par une interface « vivant / mort ». | Faible |

### 9.4 Points ouverts relevés à la relecture v1.2 (progression et sauvegarde)

| # | Constat | Conséquence | Piste | Gravité |
|---|---|---|---|---|
| P1 | Passifs de survie **stockés mais non appliqués** : `HungerThirstRate`, `HarvestYield`, `ComfortRange`, `CorruptionGain`, `FireResistance` (jauges de faim/soif, récolte, confort, corruption et résistances non codées). | Les nœuds Estomac de fer, Récolteur, Peau tannée, Volonté pure et Communion draconique s'achètent sans effet. | Brancher chaque statistique quand son système existe ; d'ici là, griser ces nœuds dans l'UI ou les retirer de `DT_SkillNodes`. | Moyenne |
| P2 | Compétences **actives non exécutées** (`bIsActive`, `PassiveStat = None`) : elles accordent seulement leur `GrantedTag`, que **aucun système ne lit** (`GrantedTags` / `HasGrantedTag` sans consommateur). | Points d'Éveil dépensés sans effet. | Système d'activation (entrée, coût, recharge, effet) à concevoir ; même remarque pour l'UI. | Moyenne |
| P3 | **Gardien et Séraphe absents** : aucun émetteur de `Event.Boss.Killed.Guardian` ni `.Seraph`. `GuardianXP`, `SeraphXP` et 2 points d'Éveil sont inaccessibles, ainsi que les glyphes secrets `S_GuardianCore` et `S_SeraphFeathers`. Surtout, **`G_DraconicForge` (non secret) exige `Event.Boss.Killed.Seraph`** : il reste `Locked` à jamais, donc la **forge draconique** (`RequiredGlyph = G_DraconicForge`) n'est pas constructible. | Une partie de l'arbre et de la construction est bloquée dans le build actuel. | Coder les deux boss, ou, pour la tranche verticale, retirer temporairement ces déclencheurs des données. | Haute (pour la tranche verticale) |
| P4 | **Pas de source d'XP courante** : seules sources codées = dragon (60 000, une fois) et volumes de découverte (XP réglée par volume). L'XP cumulée pour le niveau 80 est de l'ordre de 3,4 millions (calcul sur la formule du référentiel). Créatures, récolte, artisanat : non codés. | Montée en niveau quasi impossible ; les glyphes à `RequiredLevel` élevé sont inaccessibles. | Définir les sources d'XP au GDD ; prévoir une commande de triche (serveur, éditeur seulement) pour tester l'arbre. | Haute (pour tester) |
| P5 | **Pas de réinitialisation du boss après un wipe** (« hors périmètre » dans `EMBDragonBoss.h`) : PV, phase, mise à l'échelle figée et `bEncounterStarted` restent tels quels ; `IsEncounterActive` redevient faux (la sauvegarde reprend). Le combat n'étant pas sauvegardé, **recharger le monde remet le dragon à neuf**. | Comportement différent selon que l'on recharge ou non ; exploitation possible (affaiblir le dragon, mourir, revenir). | Réinitialisation du boss quand plus aucun joueur vivant n'est dans le rayon (avec délai), à décider au GDD avec la réapparition. | Moyenne |
| P6 | **Mort et réapparition non codées** (I13). Effet de bord de la sauvegarde : un joueur mort qui se déconnecte est enregistré PV pleins au dernier point sûr (`RespawnAtSafePoint`) et revient **vivant** à la reconnexion. | « Résurrection par déconnexion » ; en solo, quitter et relancer est le seul moyen de rejouer après une mort. | Coder la mort et la réapparition dans `AEMBGameMode` (lieu, délai, pénalité : QA8), puis revoir `DeathSaveBehavior`. | Haute |
| P7 | RPC `Server_UnlockGlyph` / `Server_RankUpSkill` : **aucun retour en cas de refus** (journal `Verbose` seulement) et **aucune limitation de fréquence**. | L'UI ne peut pas afficher le motif d'un refus serveur (désynchronisation rare mais possible) ; un client peut inonder le serveur de demandes (coût : un recalcul par demande acceptée, faible par demande refusée). | `Client_` RPC de résultat (motif) ou délégué ; anti-spam comme `Server_PlacePiece`. | Faible |
| P8 | `DeathSaveBehavior` (**à valider**) : `RespawnAtSafePoint` par défaut. | Voir P6 ; le choix dépend de la future règle de mort. | Trancher au GDD en même temps que QA8. | Moyenne |
| P9 | `SecretGlyphMode` (**à valider**) : `RevealOnly` par défaut ; `AutoUnlock` ne débloque que si niveau, parents et savoirs sont remplis (sinon révèle). | Économie des points de glyphe différente selon le mode. | Trancher au GDD (ancienne QA2). | Moyenne |
| P10 | **Sauvegarde illisible — comportement à valider.** (a) Version inconnue ou < 1 : le fichier est écarté et **toutes les écritures sont bloquées pour la session**, mais la partie démarre (monde vierge ou autre fichier) **sans aucun message au joueur** (journal seulement ; `OnSaveCompleted` n'est pas diffusé, `IsWriteBlocked` n'est lu par rien) : le joueur joue sans que rien ne soit sauvegardé. (b) Fichier que le moteur ne sait pas désérialiser : reçu comme `nullptr`, donc traité comme **absent**, **sans blocage** ; si les deux fichiers sont dans ce cas, le monde démarre vierge et la première autosave **écrase** le fichier A. (c) La détection d'un fichier tronqué par les champs de fin est à **tester** (§ 6.4). (d) `BuildPiecesTable` absente alors que le fichier contient des pièces : écritures bloquées, même silence. | Perte de progression silencieuse (a, d) ou écrasement d'un fichier peut-être récupérable (b). | Décider : refuser de démarrer le monde, ou démarrer avec un avertissement bloquant à l'écran ; dans le cas (b), renommer le fichier illisible (copie `.corrupt`) avant toute écriture. | **Haute** |
| P11 | Identifiant stable (§ 6.5) : invités identifiés par leur **nom** (non unique, change si le joueur se renomme) ; l'hôte local est toujours `LocalHost`, donc lié au **monde** et non au compte ; avec l'Online Subsystem NULL (PIE, LAN) aucun client n'a d'identifiant fiable. | Personnage perdu ou repris par un autre en LAN / tests ; acceptable avec Steam ou EOS. | Exiger un service en ligne pour la coop publique ; documenter la limite pour les tests. | Moyenne |
| P12 | Coûts **non mesurés** sur le thread de jeu : `BuildSnapshot` capture jusqu'à 15 000 pièces en une frame à chaque autosave ; `SpawnSavedPieces` crée toutes les pièces en une passe au démarrage ; tables de progression chargées en synchrone. | Hitch possible toutes les 5 minutes sur une grosse base. | Profiler (Unreal Insights) avec 5 000 et 15 000 pièces ; étaler par lots si nécessaire (pistes notées dans le code). | Moyenne |
| P13 | Report de la sauvegarde pendant une rencontre : aucune écriture tant qu'au moins un joueur vivant reste à moins de 100 m d'un dragon engagé et vivant, **sans durée maximale**. | Une rencontre très longue (ou un joueur qui reste près du dragon sans combattre) bloque l'autosave. | Plafond de report (ex. sauvegarde forcée après N minutes, hors données du combat). | Faible |
| P14 | **Rien n'a été compilé ni exécuté.** Points de compilation à surveiller en priorité : appel `SetNetUpdateFrequency` dans `AEMBPlayerState` (accesseur à confirmer sous UE 5.4), alias `using FEMBResourceStack = FEMBResourceEntry` dans un en-tête analysé par UHT, nouvelle dépendance `DeveloperSettings`. | Les statuts « Corrigé » de ce document sont des constats de lecture, pas des tests. | Compiler (Editor + Server), puis une recette PIE à 2 joueurs : achat de glyphe, mort du dragon, autosave, rechargement, reconnexion. | **Haute** |

---

## 10. Questions ouvertes

| # | Question | Qui tranche |
|---|---|---|
| QA1 | ~~Acteur par pièce ou acteur « Base » à Fast Array ?~~ **Tranché (utilisateur, v1.2) : acteur par pièce pour la v1**, Fast Array en évolution. | — |
| QA2 | Glyphe secret : l'événement **révèle** (puis achat) ou **débloque** directement ? Codé : réglage `SecretGlyphMode`, `RevealOnly` par défaut (**à valider**, P9). | GDD |
| QA3 | ~~Barème de progression~~ Fixé au référentiel v1.2 § 7.2 (niveau 80, points d'Éveil dès le niveau 2, 3 boss, découverte non partagée). Reste : **sources d'XP** hors boss (P4). | GDD |
| QA4 | ~~Personnage portable ou lié au monde ?~~ Codé : **lié au monde de l'hôte** (sauvegarde serveur seule). À confirmer par la production si un personnage portable est souhaité plus tard. | Production + GDD |
| QA5 | Reste à planifier : N1 (cumul des critiques), N4 (collision et tir ami sur les pièces), I11 (fiabilité du visuel des zones de feu), I13 / P6 (mort et réapparition), P3 (Gardien et Séraphe), P10 (sauvegarde illisible) | Dev A, Dev B, Dev C |
| QA6 | La riposte ×3 s'applique-t-elle aux boss, et se cumule-t-elle avec le critique ×3 du boss étourdi (N1) ? | GDD, puis référentiel |
| QA7 | Un joueur peut-il endommager ses propres constructions ou celles d'un coéquipier (N4) ? | GDD |
| QA8 | Réapparition : où (lit, feu de camp, point de départ), avec quelle pénalité, et qui la porte (`AEMBGameMode`) ? Conditionne `DeathSaveBehavior` (P8) et la réinitialisation du boss après un wipe (P5). | GDD, puis Dev A |
| QA9 | Sauvegarde illisible (P10) : refuser de démarrer le monde, ou démarrer avec un avertissement bloquant ? Mettre de côté un fichier non désérialisable avant de l'écraser ? | Production, puis Dev B |
| QA10 | Gardien et Séraphe (P3) : boss à coder pour la tranche verticale, ou déclencheurs retirés temporairement des données (`G_DraconicForge`, `S_GuardianCore`, `S_SeraphFeathers`) ? | Chef d'équipe + GDD |
| QA11 | Passifs de survie et compétences actives (P1, P2) : les laisser achetables sans effet, les griser, ou les retirer de `DT_SkillNodes` tant que leurs systèmes n'existent pas ? | GDD |
| QA12 | Coop publique : un service en ligne (Steam ou EOS) est-il prévu ? Sans lui, l'identifiant des clients repose sur leur nom (P11). | Production |
