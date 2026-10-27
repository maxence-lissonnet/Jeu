# Emberfall — Référentiel des constantes et contrats d'interface

> **Source unique de vérité.** Toute valeur de gameplay citée dans le GDD (`01_GDD.md`) et dans le code C++ (`Source/Emberfall/`) provient de ce fichier. En cas d'écart, c'est ce fichier qui fait foi ; corriger l'autre document.
> Toutes les durées sont exprimées en secondes **et** en frames à 60 fps (1 frame = 0,01667 s).
> Valeurs de conception à équilibrer en playtest : ce ne sont pas des mesures.

---

## 1. Conventions de code

| Élément | Règle |
|---|---|
| Module | `Emberfall` (macro d'export `EMBERFALL_API`) |
| Préfixe | `EMB` : `UEMBStatsComponent`, `AEMBDragonBoss`, `FEMBHitData`, `EEMBCombatState` |
| Tâches/services BT | `UBTTask_EMBDragonBreath`, `UBTService_EMBUpdateCombatContext`… (fichiers `BTTask_EMB*.h`) |
| Dossiers | `Source/Emberfall/Public/*.h` et `Source/Emberfall/Private/*.cpp` (à plat) ; `Emberfall.Build.cs` à la racine du module |
| Langue | identifiants en anglais, commentaires en français |
| Moteur | Unreal Engine 5.4+ ; uniquement des API réelles du moteur |
| Réseau | coop 1-4 joueurs, serveur autoritaire (listen ou dédié). La logique de jeu tourne sur le serveur (`HasAuthority()`), les clients envoient des RPC `Server`, l'état est répliqué |

## 2. Attribution des fichiers (un seul propriétaire par fichier)

| Propriétaire | Fichiers |
|---|---|
| Dev A — noyau + combat | `Emberfall.Build.cs`, `Emberfall.h/.cpp` (module + `LogEmberfall`), `EMBGameplayTags.h/.cpp`, `EMBStatsComponent.h/.cpp`, `EMBCombatTypes.h`, `EMBHitReceiver.h` (interface), `EMBCombatComponent.h/.cpp`, `EMBAnimNotifyState_ComboWindow.h/.cpp`, `EMBAnimNotifyState_IFrames.h/.cpp`, `EMBAnimNotifyState_HitWindow.h/.cpp`, `EMBProgressionTypes.h`, `EMBPlayerCharacter.h/.cpp` |
| Dev B — construction | `EMBBuildingTypes.h`, `EMBBuildingComponent.h/.cpp`, `EMBBuildablePiece.h/.cpp`, `EMBStructureSubsystem.h/.cpp`, `EMBInventoryComponent.h/.cpp` |
| Dev C — IA dragon | `EMBDragonTypes.h`, `EMBDragonBoss.h/.cpp`, `EMBDragonAIController.h/.cpp`, `EMBDragonArena.h/.cpp`, `BTTask_EMBSelectAttack.h/.cpp`, `BTTask_EMBDragonBreath.h/.cpp`, `BTTask_EMBDragonDiveAttack.h/.cpp`, `BTService_EMBUpdateCombatContext.h/.cpp`, `BTDecorator_EMBDragonPhase.h/.cpp` |
| Dev A — v1.2 progression | `EMBProgressionSettings.*`, `EMBProgressionComponent.*`, `EMBProgressionSubsystem.*`, `EMBPlayerState.*`, `EMBGameMode.*`, `EMBDiscoveryVolume.*`, `EMBCharacterMovementComponent.*`, `Config/DefaultEngine.ini`, `Config/DefaultGame.ini` |
| Dev B — v1.2 sauvegarde | `EMBSaveTypes.h`, `EMBSaveGame.*`, `EMBSaveSubsystem.*`, `Data/DT_BuildPieces.csv` |
| Chef d'équipe | `docs/00_*`, `docs/01_GDD.md`, `Data/DT_Glyphs.csv`, `Data/DT_SkillNodes.csv`, `Config/Tags/*.ini`, `README.md` |

## 3. Contrats d'interface partagés (signatures figées)

### 3.1 `UEMBStatsComponent` (Dev A) — `EMBStatsComponent.h`
Composant répliqué, utilisé par le joueur **et** par les boss (la « posture » d'un boss = sa poise).
```cpp
UENUM(BlueprintType) enum class EEMBStatType : uint8 { Health, Stamina, Poise };

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FEMBOnAttributeChanged, EEMBStatType, StatType, float, NewValue, float, MaxValue);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnDeath, AActor*, Killer);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnPoiseBroken, AActor*, Instigator);

UCLASS(ClassGroup=(Emberfall), meta=(BlueprintSpawnableComponent))
class EMBERFALL_API UEMBStatsComponent : public UActorComponent
{
public:
    float GetHealth() const;  float GetMaxHealth() const;
    float GetStamina() const; float GetMaxStamina() const;
    float GetPoise() const;   float GetMaxPoise() const;
    float GetHealthPercent() const;           // 0..1
    bool  IsDead() const;
    bool  CanAffordStamina() const;           // vrai si Stamina > 0 (règle souls : action permise tant que la jauge n'est pas vide)
    bool  ConsumeStamina(float Amount);       // false si Stamina <= 0 ; sinon soustrait (clamp 0) et relance le délai de régénération
    float ApplyDamage(float Amount, AActor* DamageCauser); // serveur ; retourne les dégâts effectifs ; déclenche OnDeath
    bool  ApplyPoiseDamage(float Amount, AActor* Instigator); // serveur ; true si la poise est brisée (puis remise à Max, OnPoiseBroken)
    void  Heal(float Amount);
    void  SetStaminaRegenMultiplier(float Multiplier); // 0.35 pendant la garde, 1.0 sinon
    void  SetMaxHealth(float NewMax, bool bRefill);    // utilisé pour la mise à l'échelle coop des boss
    UPROPERTY(BlueprintAssignable) FEMBOnAttributeChanged OnAttributeChanged;
    UPROPERTY(BlueprintAssignable) FEMBOnDeath OnDeath;
    UPROPERTY(BlueprintAssignable) FEMBOnPoiseBroken OnPoiseBroken;
};
```

### 3.2 Types de combat (Dev A) — `EMBCombatTypes.h`
```cpp
UENUM(BlueprintType) enum class EEMBCombatState : uint8 { Idle, LightAttack, HeavyAttack, ChargedAttack, Dodge, Parry, Block, Staggered, Recovery, Dead };
UENUM(BlueprintType) enum class EEMBCombatInput : uint8 { None, LightAttack, HeavyAttack, ChargedAttackStart, ChargedAttackRelease, Dodge, Parry, BlockStart, BlockEnd };
UENUM(BlueprintType) enum class EEMBHitResult : uint8 { Hit, Blocked, Parried, Dodged, GuardBroken, Killed, Ignored };
UENUM(BlueprintType) enum class EEMBDamageType : uint8 { Physical, Fire, Frost, Lightning, Corruption };

USTRUCT(BlueprintType) struct FEMBHitData
{
    TObjectPtr<AActor> Instigator = nullptr;
    float Damage = 0.f;
    float PoiseDamage = 0.f;
    float StaminaDamage = 0.f;        // coût d'endurance si l'attaque est bloquée (0 => Damage * 0.5)
    EEMBDamageType DamageType = EEMBDamageType::Physical;
    FVector HitLocation = FVector::ZeroVector;
    FVector HitDirection = FVector::ForwardVector;
    FName HitBoneName = NAME_None;    // points faibles des boss
    bool bUnblockable = false;
    bool bUnparryable = false;
    bool bDamageOverTime = false;     // ajout v1.1 : lave, brûlure, zones de feu -> dégâts directs, ni esquive, ni parade, ni counter-hit, ni multicast
    FGameplayTag AttackTag;
};
```

### 3.3 Interface `IEMBHitReceiver` (Dev A) — `EMBHitReceiver.h`
Interface C++ native (non implémentable en Blueprint) pour les acteurs qui veulent traiter eux-mêmes un coup (boss : points faibles, invulnérabilité de transition de phase).
```cpp
UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint))
class UEMBHitReceiver : public UInterface { GENERATED_BODY() };
class EMBERFALL_API IEMBHitReceiver
{
    GENERATED_BODY()
public:
    virtual EEMBHitResult ReceiveEMBHit(const FEMBHitData& Hit) = 0;
};
```

### 3.4 `UEMBCombatComponent` (Dev A)
```cpp
EEMBHitResult ReceiveHit(const FEMBHitData& Hit);                       // serveur : esquive / parade / garde / dégâts
static EEMBHitResult ApplyHitToActor(AActor* Target, const FEMBHitData& Hit);
// Routage : 1) Target implémente IEMBHitReceiver -> ReceiveEMBHit ; 2) sinon UEMBCombatComponent -> ReceiveHit ;
//           3) sinon UEMBStatsComponent -> ApplyDamage + ApplyPoiseDamage ; 4) sinon Ignored.
void HandleInput(EEMBCombatInput Input);                                // appelé par le personnage (Enhanced Input)
EEMBCombatState GetCurrentState() const;
bool IsInvulnerable() const;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnCombatStateChanged, EEMBCombatState, OldState, EEMBCombatState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FEMBOnParrySuccess, AActor*, Attacker);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FEMBOnHitReceived, const FEMBHitData&, Hit, EEMBHitResult, Result);
```

### 3.5 `UEMBInventoryComponent` (Dev B)
```cpp
int32 GetResourceCount(FName ResourceId) const;
bool  HasResources(const TMap<FName, int32>& Cost) const;
bool  ConsumeResources(const TMap<FName, int32>& Cost);   // serveur, atomique (tout ou rien)
void  AddResource(FName ResourceId, int32 Amount);         // serveur
```
Identifiants de ressources (`FName`) : `Wood`, `Stone`, `Fiber`, `Flint`, `IronOre`, `IronIngot`, `BlackSteelIngot`, `Obsidian`, `MoonSilver`, `DragonScale`, `DragonBone`, `EmberHeart`.

### 3.6 `UEMBBuildingComponent` (Dev B) — API appelée par `AEMBPlayerCharacter`
```cpp
void ToggleBuildMode();
void SetSelectedPiece(FName PieceRowName);   // ligne de DT_BuildPieces
void RotatePreview(float Direction);          // +1 / -1
void ConfirmPlacement();                      // appelle un RPC Server
void CancelBuildMode();
bool IsInBuildMode() const;
```

### 3.7 Gameplay Tags natifs (Dev A) — `EMBGameplayTags.h`
Déclarés dans `namespace EMBTags { EMBERFALL_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Nom); }`, définis avec `UE_DEFINE_GAMEPLAY_TAG_COMMENT`.

| Variable C++ | Tag |
|---|---|
| `State_Combat_Invulnerable` | `State.Combat.Invulnerable` |
| `State_Combat_Staggered` | `State.Combat.Staggered` |
| `State_Combat_HyperArmor` | `State.Combat.HyperArmor` |
| `State_Dead` | `State.Dead` |
| `Attack_Player_Light` / `_Heavy` / `_Charged` | `Attack.Player.Light` / `.Heavy` / `.Charged` |
| `Attack_Dragon_Bite`, `_ClawSwipe`, `_TailSweep`, `_WingBuffet`, `_BreathCone`, `_BreathSweep`, `_Dive`, `_AerialStrafe`, `_EmberRain`, `_Grab`, `_Cataclysm` | `Attack.Dragon.Bite`, `.ClawSwipe`, `.TailSweep`, `.WingBuffet`, `.BreathCone`, `.BreathSweep`, `.Dive`, `.AerialStrafe`, `.EmberRain`, `.Grab`, `.Cataclysm` |
| `Boss_Phase_1` / `_2` / `_3` | `Boss.Phase.1` / `.2` / `.3` |
| `Building_Material_Wood` / `_Stone` / `_Iron` / `_Draconic` | `Building.Material.Wood` / `.Stone` / `.Iron` / `.Draconic` |
| `Building_Category_Foundation` / `_Floor` / `_Wall` / `_Roof` / `_Pillar` / `_Stairs` / `_Furniture` / `_Workstation` | `Building.Category.*` |
| `Building_NoBuildZone` | `Building.NoBuildZone` |
| `Event_Boss_Killed_Guardian` / `_Seraph` / `_Dragon` | `Event.Boss.Killed.Guardian` / `.Seraph` / `.Dragon` |
| `Event_Location_Discovered_SunkenLibrary` / `_AshAltar` / `_StarWell` | `Event.Location.Discovered.SunkenLibrary` / `.AshAltar` / `.StarWell` |

---

## 4. Constantes du joueur et du combat

### 4.1 Attributs de base (niveau 1, sans équipement)
| Constante | Valeur |
|---|---|
| `MaxHealth` | 500 |
| `MaxStamina` | 100 |
| `MaxPoise` | 50 |
| `StaminaRegenRate` | 30 /s |
| `StaminaRegenDelay` (après toute consommation) | 0,8 s (48 f) |
| `ExhaustedRegenDelay` (jauge tombée à 0) | 1,5 s (90 f) |
| `BlockStaminaRegenMultiplier` | 0,35 |
| `PoiseRegenRate` | 15 /s |
| `PoiseRegenDelay` (après le dernier coup reçu) | 2,0 s |
| `InputBufferTime` | 0,2 s (12 f) |
| Sprint (GDD, hors FSM) | 8 endurance/s ; saut : 10 |

### 4.2 Actions (arme de référence : épée longue, dégâts de base `WeaponBaseDamage` = 80)
| Action | Endurance | Multiplicateur de dégâts | Dégâts de poise | Durée totale | Fenêtres (frames à 60 fps) |
|---|---|---|---|---|---|
| Attaque légère (combo 3 coups) | 14 | ×1,0 (3e coup ×1,2) | 12 | 0,60 s (36 f) | startup 0-11, actif 12-17, recovery 18-35 ; **fenêtre de combo 18-30** (0,30-0,50 s) |
| Attaque lourde | 26 | ×1,45 | 25 | 1,00 s (60 f) | startup 0-23, actif 24-31, recovery 32-59 ; hyper-armure 6-31 |
| Attaque chargée | 38 (au relâchement) | ×1,6 → ×2,4 (linéaire) | 35 → 55 | charge 0,4 s mini, 1,2 s maxi + frappe 0,9 s (54 f) | relâché avant 0,4 s → attaque lourde ; **relâchement automatique à 1,2 s** ; annulable par une roulade pendant la charge (endurance non dépensée) ; la charge compte comme startup (counter-hit ×1,25) ; hyper-armure pendant la charge et l'actif |
| Roulade | 18 | — | — | 0,70 s (42 f) | **i-frames 3-15 (13 f = 0,217 s)** ; annulable à partir de 0,50 s (frame 30) |
| Parade | 10 | — | posture infligée à l'attaquant : 35 | 0,75 s (45 f) | **fenêtre parfaite 4-13 (10 f = 0,167 s)** ; parade ratée : vulnérable 14-44 |
| Garde (maintenue) | coût par coup bloqué = `StaminaDamage` (0 → `Damage`×0,5) | réduction (bouclier) : physique 85 %, feu/givre/foudre 50 %, corruption 30 % | — | levée en 0,10 s (6 f) | régénération ×0,35 |
| Rupture de garde | — | — | — | Staggered 1,2 s (72 f) | si l'endurance tombe à 0 en garde |
| Poise brisée | — | — | — | Staggered 0,8 s (48 f) | — |

Charge de poids (i-frames de roulade, montages différents) : léger < 30 % → 13 f (3-15) ; moyen 30-70 % → 11 f (3-13) ; lourd 70-100 % → 9 f (3-11), coût 22 ; surchargé > 100 % → pas de roulade.

### 4.3 Punitions et modificateurs
| Constante | Valeur |
|---|---|
| `CounterHitDamageMultiplier` (coup reçu en Recovery, parade ratée ou startup d'attaque) | ×1,25 |
| `HyperArmorPoiseMultiplier` (poise subie pendant l'hyper-armure) | ×0,5 |
| `ParryPostureDamage` (posture infligée à l'attaquant sur parade parfaite) | 35 |
| Fenêtre de riposte sur ennemi standard paré | 1,5 s |
| `BossPostureBreakDuration` (étourdissement quand la posture d'un boss est brisée) | 4,0 s |
| Coup critique (riposte sur boss étourdi, ou premier coup sur l'attaquant paré pendant la fenêtre de riposte) | ×3,0 |
| Canal de trace de mêlée | `EMBMeleeHit` = `ECC_GameTraceChannel1` (ignore par défaut ; Physics Assets et pawns en Overlap ; décor en Block ; capsules ignorées) |
| Canal de placement | `EMBBuilding` = `ECC_GameTraceChannel2` (pièces Block ; volumes de zone interdite Overlap) |
| Canal des projectiles (v1.3) | `EMBProjectile` = `ECC_GameTraceChannel3` (objet ; Block par défaut pour que pawns et décor acceptent le projectile ; le projectile règle Block WorldStatic, Overlap Pawn, Ignore WorldDynamic et lui-même) |
| Charge (poids porté, ressources seules en v1) | maximum 300 ; poids unitaires : Wood 0,5 · Stone 1 · Fiber 0,1 · Flint 0,5 · IronOre 2 · IronIngot 1 · BlackSteelIngot 1 · Obsidian 1,5 · MoonSilver 1 · DragonScale 2 · DragonBone 3 · EmberHeart 5 |

---

## 5. Construction

| Constante | Valeur |
|---|---|
| `GridSize` | 300 cm (fondation 300×300×50) |
| `WallHeight` | 300 cm |
| `MaxPlacementDistance` | 800 cm (depuis le personnage) |
| `MaxFoundationSlopeDeg` | 30° |
| `GroundTraceDepth` | 180 cm (v1.1 : une fondation de 300 cm sur une pente de 30° doit pouvoir s'enfoncer de 173 cm ; la pente de 30° prime) |
| Pas de grille par catégorie | fondation, sol, toit : 300 cm ; mur, pilier, escalier : 150 cm ; mobilier, poste de travail : 25 cm (validé aussi côté serveur, ±5 cm, ou coïncidence avec un socket) |
| Limites | 3 000 pièces par base (composante connexe), 15 000 par monde |
| Tolérance serveur de distance | +150 cm (latence) → 950 cm effectifs côté serveur |
| Délai de grâce des pièces pré-placées / streamées | 1,0 s avant la première évaluation d'intégrité |
| `SocketSnapRadius` | 150 cm |
| `OverlapBoxShrink` | 0,95 (boîte réduite de 5 % pour tolérer les contacts entre pièces voisines) |
| `RotationStepSnapped` / `RotationStepFree` | 90° / 15° (mobilier) |
| `FurnitureSnap` | 25 cm |
| `GroundedIntegrity` | 100 |
| `CollapseThreshold` | intégrité ≤ 0 → effondrement |
| `CollapseWaveDelay` | 0,08 s entre deux niveaux de propagation |
| `MaxCollapsePerFrame` | 32 pièces |
| `PillarVerticalDecayMultiplier` | ×0,5 (un pilier perd moitié moins vers le haut) |
| Couleur de l'aperçu | vert > 50 ; jaune 25-50 ; orange 1-24 ; rouge = invalide |

Intégrité d'une pièce = max sur ses supports (intégrité du support − décroissance de **son propre** matériau selon la direction). Verticale si la pièce est au-dessus de son support (Δz > 50 cm), horizontale sinon.

| Matériau | Tier | PV de la pièce | Décroissance verticale | Décroissance horizontale | Portée indicative |
|---|---|---|---|---|---|
| Bois | T1 | 500 | 10 | 25 | 9 niveaux / 3 travées en porte-à-faux |
| Pierre | T2 | 1 500 | 6 | 20 | 16 niveaux / 4 travées |
| Fer forgé | T3 | 3 000 | 5 | 12 | 19 niveaux / 8 travées |
| Draconique | T4 | 6 000 | 3 | 8 | 33 niveaux / 12 travées |

Coûts (mur standard) : bois 10 `Wood` ; pierre 12 `Stone` + 2 `Wood` ; fer 6 `IronIngot` + 6 `Stone` ; draconique 2 `DragonBone` + 8 `Obsidian` + 4 `IronIngot`. Fondation = coût du mur ×2 ; sol/plafond = ×1 ; toit = ×1 ; pilier = ×0,5 (arrondi supérieur).

---

## 6. Dragon final — Vaelthyrax, l'Ancêtre-Braise

| Constante | Valeur |
|---|---|
| PV (solo) | 90 000 |
| Mise à l'échelle coop | PV × (1 + 0,6 × (joueurs − 1)) |
| Posture (`MaxPoise`) | 1 500 ; régénération 40/s après 4 s sans dégâts de posture |
| Étourdissement de posture | 4,0 s, riposte critique ×3 |
| Seuils de phase | P2 à 70 % des PV, P3 à 35 % |
| Transition de phase | invulnérable 5,0 s (rugissement), puis changement d'arène |
| Enragé (P3) | vitesse des montages ×1,15 ; recharges ×0,8 ; télégraphes ×0,9 |
| Vitesses | marche 600 cm/s, vol 2 400 cm/s, piqué 4 500 cm/s |
| Points faibles (multiplicateur selon l'os touché) | tête ×1,3 ; ventre ×1,15 ; ailes ×1,0 ; queue ×0,8 |
| Sélection d'attaque | score = poids de base × facteur distance × facteur angle × poids de phase × pénalité de répétition (dernière attaque ×0,25 ; avant-dernière ×0,6) ; attaques en recharge exclues |

| ID (`EMBTags`) | Phases | Portée (cm) | Angle | Télégraphe | Fenêtre d'esquive (s depuis le début) | Dégâts | Poise | Parable | Poids | Recharge |
|---|---|---|---|---|---|---|---|---|---|---|
| Bite (morsure) | 1-2-3 | 0-900 | ±45° avant | 0,9 s (tête qui recule) | 0,80-1,05 | 220 phys. | 60 | **Oui** | 30 | 3 s |
| ClawSwipe (griffe, 2 coups dont un retardé) | 1-2-3 | 0-1 100 | ±90° | 1,1 s (patte levée) | 1,00-1,25 puis 1,70-1,95 | 260 ×2 phys. | 70 | Non | 25 | 4 s |
| TailSweep (fouet caudal) | 1-2-3 | 0-1 400 | arrière (> 110°) | 0,7 s (queue raidie) | 0,60-0,85 | 300 phys. | 90 | Non | 35 | 5 s |
| WingBuffet (rafale d'ailes) | 1-2-3 | 0-1 600 | 360° | 1,2 s (ailes déployées) | 1,10-1,40 (repousse 1 200 cm) | 80 phys. | 40 | Non | 15 (×3 si ≥ 2 joueurs à < 800 cm) | 10 s |
| BreathCone (souffle frontal) | 1-2-3 | 1 000-3 500 | ±30° | 1,6 s (gorge incandescente) | sortir du cône dès 1,4 ; souffle 2,5 s, cône 20° | 35 feu/tick, 8 ticks/s + brûlure 15/s 4 s | 10/tick | Non | 25 | 12 s |
| BreathSweep (souffle balayant) | 2-3 | 600-3 000 | ±60° | 1,8 s | rouler à travers au passage (arc 120° en 2,0 s) | 40 feu/tick | 10/tick | Non | 20 | 16 s |
| Dive (piqué) | 2-3 | 1 500-6 000 | tout | envol 2,0 s + ombre 1,5 s | flash 0,35 s avant l'impact | 400 phys. rayon 600 | 120 | Non | 20 | 18 s ; **vulnérable 2,5 s après l'impact** |
| AerialStrafe (souffle en vol rasant) | 2-3 | 2 000-8 000 | tout | 2,0 s | quitter la ligne ; zones de feu 8 s à 20/s | 30 feu/tick | 10/tick | Non | 15 | 20 s |
| EmberRain (pluie de braises) | 3 | tout | tout | 2,5 s (rugissement) ; cercles 1,2 s avant chaque impact | 12 impacts en 6 s, rayon 300 | 180 feu | 40 | Non | 15 | 25 s |
| Grab (saisie) | 3 | 0-800 | ±30° | 1,0 s (rapide) | 0,85-1,10 (esquive tardive) | 350 + brûlure | 150 | Non, **imblocable** | 15 | 14 s |
| Cataclysm (embrasement total) | 3 | tout | tout | 4,0 s de charge en vol | se cacher derrière un obstacle (ligne de vue) | 900 feu | 200 | Non, imblocable | scripté : entrée en P3 + 20 s, puis toutes les 75 s | 75 s |

Arène (caldera de Cendrehaute) : P1 intacte (6 piliers de basalte, 3 ponts) ; P2 coulées de lave (fissures 25 dégâts/s), 3 piliers détruits, envol fréquent ; P3 effondrement du pourtour → île centrale de rayon 4 000 cm, tempête de cendres et foudre (impact 150, télégraphe 1,5 s, rayon 250 cm, un éclair toutes les 3,0 s sur la position d'un joueur au hasard décalée de 0 à 400 cm).

Compléments v1.1 (arbitrages du chef d'équipe après revue) :
- `VulnerableDamageMultiplier` (2,5 s après le piqué) = **×1,5**.
- Ticks de souffle : `StaminaDamage` = 10 par tick (garde tenable environ 1,2 s face au souffle).
- Lave, brûlure, zones de feu, foudre : `bDamageOverTime = true` sauf foudre (coup normal, esquivable).
- Mise à l'échelle coop figée au début de la rencontre (premier joueur détecté ou premier dégât) ; un joueur qui arrive après ne change pas les PV.
- Les arènes de boss sont des zones `Building.NoBuildZone` : le dragon n'a pas à endommager de construction.
- Les multiplicateurs critiques ne se cumulent pas : la riposte ×3 du joueur ne s'applique pas à une cible déjà étourdie (critique ×3 du boss). Les points faibles et la vulnérabilité, eux, se multiplient.
- Coop PvE : les joueurs n'endommagent pas les constructions à l'arme (`bAllowPlayerStructureDamage = false`, option serveur pour un futur PvP) ; les pièces ignorent le canal `EMBMeleeHit`.
- Foudre de l'arène : instigateur = le boss (orientation de la garde), plus d'éclair après sa mort.

---

## 7. Progression (v1.2)

### 7.1 Arbitrage d'architecture
- **État par joueur** : `UEMBProgressionComponent` sur `AEMBPlayerState` (répliqué). Le PlayerState survit à la mort du pawn, existe sur le serveur pour tous les joueurs et se réplique ; un `UGameInstanceSubsystem` ne se réplique pas.
- **Règles et routage** : `UEMBProgressionSubsystem : UWorldSubsystem` sans état de joueur (DataTables, événements monde `Event.*`, distribution aux joueurs concernés), actif sur le serveur.
- **Données** : `UEMBProgressionSettings : UDeveloperSettings` (`Config=Game`, section dans `DefaultGame.ini`) : `TSoftObjectPtr<UDataTable> GlyphTable` (`DT_Glyphs`), `SkillTable` (`DT_SkillNodes`), `SecretGlyphMode`, `DeathSaveBehavior`, paramètres XP.
- **Autorité** : toute modification (XP, achat, déclencheur) se fait sur le serveur. Le client appelle des RPC `Server` du composant (le PlayerState appartient à la connexion du PlayerController : RPC autorisées).

### 7.2 Constantes
| Constante | Valeur |
|---|---|
| `MaxLevel` | 80 |
| XP pour passer du niveau L à L+1 | `round(150 × L^1,5)` (L=1 : 150 ; L=20 : 13 416 ; L=79 : ≈ 105 300) |
| Points de glyphe gagnés en atteignant le niveau L | L 2-20 : 4 ; 21-50 : 6 ; 51-80 : 8 → **496** au niveau 80 |
| Points d'Éveil | 1 par niveau gagné (79) + 1 par boss majeur (`Event.Boss.Killed.Guardian/.Seraph/.Dragon`, une seule fois chacun) → **82** |
| XP des boss | Gardien 6 000 ; Séraphe 15 000 ; Dragon 60 000 (à chaque joueur participant) |
| Participants d'un boss | joueurs (vivants ou morts) dans un rayon de 10 000 cm du boss à sa mort |
| `SecretGlyphMode` (**à valider**) | `RevealOnly` par défaut (le déclencheur révèle le glyphe, il reste à acheter) ; `AutoUnlock` (débloqué gratuitement si les autres prérequis — niveau, parents, savoirs — sont remplis, sinon seulement révélé) — réglable dans `DefaultGame.ini`, section `[/Script/Emberfall.EMBProgressionSettings]` |
| Source des XP de boss | `UEMBProgressionSettings` uniquement (`GuardianXP`, `SeraphXP`, `DragonXP`, `ParticipantRadius`) ; un événement monde déjà enregistré ne redonne pas d'XP |
| Déclencheurs | boss vaincu : tous les participants ; lieu découvert : le joueur qui entre dans le volume (`AEMBDiscoveryVolume`) |
| Savoirs (`RequiredKnowledge`) | `FName` ajoutés au joueur par `GrantKnowledge` (objets de lore, volumes) |

### 7.3 Passifs du Skill Tree
`FEMBSkillNodeRow` reçoit trois colonnes en fin de ligne : `EEMBPassiveStat PassiveStat`, `float ValuePerRank` et `TArray<FName> RequiredKnowledge` (v1.2 après revue : « Communion draconique » exige `Lore_DraconicTongue`). Les modificateurs s'additionnent par rang ; un multiplicateur vaut `max(0, 1 + somme)`. Le composant agrège les rangs en `FEMBPassiveModifiers` et les **pousse** dans les composants du pawn (setters ajoutés, contrats existants inchangés) à chaque changement et à chaque possession d'un pawn.

| `EEMBPassiveStat` | Nœud GDD | Valeur/rang | Appliqué à |
|---|---|---|---|
| `PhysicalDamage` | Poigne d'acier | +0,04 | Combat (multiplicateur de dégâts physiques) |
| `AttackStaminaCost` | Souffle du duelliste | −0,05 | Combat (coût des attaques) |
| `RiposteMultiplier` | Riposte sanglante | +0,5 | Combat (×3 → ×3,5) |
| `DodgeIFrames` | Pas fantôme | +2 frames | Combat |
| `GuardStaminaCost` | Garde inébranlable | −0,15 | Combat |
| `ParryWindowFrames` | Parade du maître | +2 frames | Combat (10 → 12) |
| `BuildCost` | Bâtisseur | −0,10 | Building (coût arrondi au supérieur, minimum 1) |
| `CarryWeight` | Bête de somme | +30 | Inventory |
| `HungerThirstRate`, `HarvestYield`, `ComfortRange`, `CorruptionGain`, `FireResistance` | Estomac de fer, Récolteur, Peau tannée, Volonté pure, Communion draconique | voir GDD §8.2 | **stockés et exposés, non appliqués** (jauges de survie, récolte et résistances non codées) |
| `None` | compétences actives | — | accordent leur `GrantedTag` ; l'exécution des actives n'est pas codée |

### 7.4 Signatures figées
```cpp
// EMBProgressionTypes.h (ajouts)
UENUM(BlueprintType) enum class EEMBPassiveStat : uint8 { None, PhysicalDamage, AttackStaminaCost, RiposteMultiplier, DodgeIFrames, GuardStaminaCost, ParryWindowFrames, BuildCost, CarryWeight, HungerThirstRate, HarvestYield, ComfortRange, CorruptionGain, FireResistance };
UENUM(BlueprintType) enum class EEMBSecretGlyphMode : uint8 { RevealOnly, AutoUnlock };
UENUM(BlueprintType) enum class EEMBDeathSaveBehavior : uint8 { RespawnAtSafePoint, KeepPosition };
UENUM(BlueprintType) enum class EEMBGlyphState : uint8 { Hidden, Locked, Available, Unlocked };
USTRUCT(BlueprintType) struct FEMBPassiveModifiers { /* un float par EEMBPassiveStat, neutre par défaut */ };
USTRUCT(BlueprintType) struct FEMBSkillRank { FName NodeId; int32 Rank = 0; };
USTRUCT(BlueprintType) struct FEMBProgressionSaveData
{
    int32 Level = 1; int64 TotalXP = 0;            // XP cumulée depuis le niveau 1
    int32 UnspentGlyphPoints = 0; int32 UnspentAwakeningPoints = 0;
    TArray<FName> UnlockedGlyphs; TArray<FName> RevealedGlyphs;
    TArray<FEMBSkillRank> SkillRanks;
    TArray<FName> Knowledge;
    FGameplayTagContainer ObtainedTriggers;        // Event.* acquis par ce joueur
};

// UEMBProgressionComponent (sur AEMBPlayerState)
void AddXP(int64 Amount);                                        // serveur
void GrantTrigger(FGameplayTag EventTag);                         // serveur
void GrantKnowledge(FName KnowledgeId);                           // serveur
EEMBGlyphState GetGlyphState(FName GlyphId) const;
bool CanUnlockGlyph(FName GlyphId, FText& OutReason) const;
void RequestUnlockGlyph(FName GlyphId);                           // client -> Server_UnlockGlyph
bool CanRankUpSkill(FName NodeId, FText& OutReason) const;
void RequestRankUpSkill(FName NodeId);                            // client -> Server_RankUpSkill
bool HasGlyph(FName GlyphId) const;
bool HasGrantedTag(FGameplayTag Tag) const;
const FEMBPassiveModifiers& GetPassiveModifiers() const;
void ApplyModifiersToPawn(APawn* Pawn);                           // serveur
void ExportSaveData(FEMBProgressionSaveData& Out) const;
void ImportSaveData(const FEMBProgressionSaveData& In);           // serveur ; recalcule et réapplique les modificateurs
// Délégués (UI) : OnXPChanged(int64 TotalXP, int32 Level), OnLevelUp(int32 NewLevel), OnPointsChanged(int32 Glyph, int32 Awakening),
//                 OnGlyphStateChanged(FName GlyphId, EEMBGlyphState NewState), OnSkillRankChanged(FName NodeId, int32 NewRank)

// UEMBProgressionSubsystem (UWorldSubsystem, serveur)
void NotifyWorldEvent(FGameplayTag EventTag, AActor* Source, int64 XPReward, float ParticipantRadius);
void NotifyPlayerEvent(APlayerState* Player, FGameplayTag EventTag, int64 XPReward);
const FEMBGlyphRow* FindGlyph(FName GlyphId) const;
const FEMBSkillNodeRow* FindSkill(FName NodeId) const;
bool IsWorldEventRecorded(FGameplayTag EventTag) const;
void RestoreWorldEvents(const FGameplayTagContainer& Events);     // chargement
// Délégué : OnWorldEventRecorded(FGameplayTag EventTag) — écouté par la sauvegarde (autosave sur boss vaincu)

// Ajouts aux composants existants (ajouts uniquement, contrats précédents inchangés)
UEMBCombatComponent::SetPassiveModifiers(const FEMBPassiveModifiers&);
UEMBBuildingComponent::SetBuildCostMultiplier(float);   // IsPieceUnlocked consulte HasGlyph(Row.RequiredGlyph)
UEMBInventoryComponent::SetCarryWeightBonus(float);
UEMBInventoryComponent::ExportResources(TArray<FEMBResourceStack>& Out) const;  // type de paire existant ou équivalent
UEMBInventoryComponent::ImportResources(const TArray<FEMBResourceStack>& In);  // serveur
UEMBStatsComponent::RestoreFromSave(float Health, float Stamina);               // serveur
```
`FEMBBuildPieceRow` reçoit une colonne `FName RequiredGlyph` (vide = toujours disponible ; bois `G_WoodFrame`, pierre `G_StoneMasonry`, fer `G_IronFrame`, draconique `G_DraconicArchitecture`, coffre `G_StorageChest`, lit `G_Bed`, feu de camp `G_Campfire`, établi `G_Workbench`, forge `G_Forge`, fonderie `G_Smelting`, forge draconique `G_DraconicForge`).

## 8. Sauvegarde (v1.2)

| Constante | Valeur |
|---|---|
| `SaveFormatVersion` | **2** depuis la v1.3 (1 = v1.2) ; incrémenté à chaque changement de format ; migration par étapes N → N+1 ; résultat `Ok` / `TooRecent` (bloque les écritures) / `Invalid` (fichier ignoré sans bloquer) |
| Ajouts v2 | joueur : survie (`FEMBSurvivalSaveData`, importée seulement si `bHasSurvivalState`), `BoundRespawnPieceSaveId` ; monde : sacs de mort (propriétaire stable, transform, contenu) ; boss : itération sur `AEMBBossBase` |
| Piège de sérialisation | une valeur égale au défaut n'est pas écrite : les champs sauvegardés ont des défauts neutres (0) et non des constantes d'équilibrage |
| Slots | 3 mondes `EMB_World_0` à `EMB_World_2` ; chaque monde écrit en alternance `_A` / `_B` et charge le plus récent valide (une écriture interrompue ne détruit jamais la précédente) |
| Autosave | toutes les 300 s (serveur), à la mort d'un boss, à la déconnexion d'un joueur ; jamais pendant une rencontre de boss active (reportée à la fin) |
| Écriture | `UGameplayStatics::AsyncSaveGameToSlot` ; une seule écriture en vol (une demande pendant l'écriture est mise en attente) |
| Qui sauvegarde | uniquement le serveur (hôte ou dédié) |
| Identifiant stable d'un joueur | `APlayerState::GetUniqueId().ToString()` (Steam/EOS : stable entre sessions). **Limites** : avec l'Online Subsystem NULL (PIE, LAN) il peut changer d'une session à l'autre ; repli : l'hôte local reçoit la clé fixe `LocalHost`, un client sans identifiant valide reçoit `Guest_<nom du joueur>` (non garanti unique). Un identifiant inconnu démarre un personnage neuf. |
| `DeathSaveBehavior` (**à valider**) | `RespawnAtSafePoint` par défaut : un joueur mort au moment de la sauvegarde est enregistré PV et endurance pleins à son dernier point sûr (dernière position vivante hors combat, relevée toutes les 10 s), sinon au PlayerStart ; `KeepPosition` : position telle quelle. Aucune pénalité de mort codée. |

Compléments v1.2 après revue :
- **Cycle de vie** : `AEMBGameMode::InitGame` appelle `BindToWorld(World)` (synchrone, remise à zéro de l'état du monde précédent) puis `SetActiveSlot`, `StartPlay` appelle `LoadActiveSlot` (idempotent, protégé par un jeton de génération), `PostLogin` → `RestorePlayer`, `Logout` → `CapturePlayer` avant la destruction du pawn, `EndPlay` → `SaveNowBlocking()` (écriture synchrone à l'arrêt de l'hôte ou du serveur).
- **Fichier inutilisable** (version supérieure au binaire, migration impossible) : aucune écriture tant que le problème n'est pas réglé (le monde n'est pas écrasé) ; message d'erreur dans le journal. **À valider** : refuser de démarrer plutôt que lancer un monde vierge.
- **Fichier tronqué** : derniers champs `ExpectedPlayerCount`, `ExpectedPieceCount`, `bComplete` ; un fichier incohérent est ignoré.
- **Bornes à l'import** : pièces (échelle 1, coordonnées ≤ 2 097 152 cm, ligne existante, matériau et catégorie relus dans la table, au plus 15 000), joueurs ≤ 256, ressources connues seulement, progression recalculée (niveau déduit de l'XP, points plafonnés, glyphes et nœuds inconnus ignorés).
- **Identifiant déjà connecté** (deux invités de même nom) : le second démarre un personnage neuf non sauvegardé.
- **Journaux** : identifiants hachés, aucun identifiant de plateforme ni nom en clair.
- **Dernier point sûr** : pawn vivant, au sol, en état Idle, hors rencontre de boss.

Contenu :
- **Par joueur** (`FEMBPlayerSaveData`, clé = identifiant stable) : progression, inventaire, PV, endurance, transform du pawn, dernier point sûr.
- **Monde** (`FEMBWorldSaveData`) : événements monde acquis (`Event.Boss.Killed.*` : un boss vaincu ne réapparaît pas).
- **Constructions** (`FEMBPieceSaveData`) : identifiant de sauvegarde, ligne `DT_BuildPieces`, classe (soft path), transform, PV, matériau, ancrage, identifiant stable du poseur, liens de support (identifiants des voisins, utilisés pour contrôle). Chargement en masse (`BeginBulkLoad` / `EndBulkLoad` du subsystem de structure : ni effondrement ni recalcul pendant l'enregistrement), puis un recalcul global de l'intégrité ; les pièces à intégrité ≤ 0 sont détruites **sans effet ni vagues** au chargement (rien de spectaculaire à l'arrivée des joueurs).

---

## 9. Mort, réapparition, sac, réanimation (v1.3)

### 9.1 Attribution v1.3 (un propriétaire par fichier ; le dépôt git sert de sauvegarde, plus de `.bak`)
| Propriétaire | Fichiers |
|---|---|
| Dev A — mort / réanimation / interaction | `EMBInteractable.h` (nouveau), `EMBPlayerCharacter.*`, `EMBCombatComponent.*`, `EMBCombatTypes.h`, `EMBStatsComponent.*`, `EMBGameMode.*`, `EMBGameplayTags.*`, `Config/DefaultEngine.ini` |
| Dev B — sac / points de réapparition / sauvegarde v2 | `EMBDeathBag.*` (nouveau), `EMBRespawnSubsystem.*` (nouveau), `EMBBuildablePiece.*`, `EMBBuildingTypes.h`, `EMBBuildingComponent.*`, `EMBInventoryComponent.*`, `EMBStructureSubsystem.*`, `EMBSaveTypes.h`, `EMBSaveGame.*`, `EMBSaveSubsystem.*`, `Data/DT_BuildPieces.csv`, `Config/DefaultGame.ini` (sections de ses classes) |
| Dev D — survie | `EMBSurvivalTypes.h`, `EMBSurvivalComponent.*`, `EMBEnvironmentSubsystem.*`, `EMBEnvironmentVolume.*` (nouveaux), `Data/DT_Consumables.csv` |
| Dev C — boss | `EMBBossBase.*`, `EMBBossTags.*`, `EMBColossusBoss.*`, `EMBColossusArena.*` (nouveaux), fichiers dragon et BT existants, `Data/DT_ColossusAttacks.csv` |
| Dev E — boss 2 | `EMBSeraphBoss.*`, `EMBSeraphArena.*`, `EMBProjectile.*`, tâches BT propres au Séraphe (nouveaux), `Data/DT_SeraphAttacks.csv` |
| Dev A (progression, inchangé) | `EMBProgression*`, `EMBPlayerState.*`, `EMBDiscoveryVolume.*` |

### 9.2 Constantes
| Constante | Valeur |
|---|---|
| Délai minimal avant de pouvoir demander la réapparition | 3,0 s après la mort, en solo **et en coop** (arbitrage v1.3 ; option `bRespawnRequiresNoLivingAlly` pour imposer l'attente de 30 s en coop) |
| Réapparition automatique | 30,0 s après la mort (fenêtre de réanimation en coop) |
| Valeurs après réanimation | Corruption plafonnée à 60, intoxication purgée (sinon un joueur mort de Corruption 100 re-mourrait) |
| Lit « Dormir » | Corruption −50 au plus une fois toutes les 120 s par joueur ; la liaison reste instantanée |
| Mort dans la lave (coup `bLethalZone`) ou chute hors du monde | sac déposé au dernier point sûr connu ; toute autre mort (faim, froid, brûlure…) laisse le sac sur place |
| Réanimation après une mort de faim ou de soif | faim et soif remontées à au moins 30 |
| Points de réapparition pré-placés dans le niveau | utilisables mais non sauvegardés (seules les pièces posées par un joueur ont un `SaveId`) |
| Réanimation par un allié | maintenir *Interagir* 4,0 s à ≤ 250 cm du joueur mort (annulée si l'allié s'éloigne, meurt ou est étourdi) ; le joueur revient à **30 % des PV**, 50 % d'endurance, sans perdre son inventaire, avec 3,0 s d'invulnérabilité |
| Sac (`AEMBDeathBag`) | déposé au moment de la **réapparition** (pas si réanimé) ; contient **toutes les ressources** de l'inventaire ; l'XP et la progression ne sont jamais perdues ; un seul sac actif par joueur : mourir avant de le récupérer détruit l'ancien sac et son contenu ; seul son propriétaire peut le ramasser (interaction) ; position = lieu de la mort, ou dernier point sûr si la mort a lieu hors sol (chute, lave, vide) ; sauvegardé |
| Point de réapparition | pièce construite dont la ligne `DT_BuildPieces` a `bIsRespawnPoint = True` (lit, feu de camp) ; le joueur s'y **lie** par interaction (le dernier lien remplace le précédent) ; sauvegardé par `SaveId` de pièce ; pièce détruite → lien perdu → `PlayerStart` |
| Position de réapparition | point lié valide (décalé de 150 cm devant la pièce, sur le sol) → sinon `PlayerStart` |
| Valeurs à la réapparition | PV et endurance pleins, poise pleine, faim 60, soif 60, Corruption 0, température corporelle neutre |
| Invulnérabilité de réapparition | 3,0 s (tag `State.Combat.Invulnerable`), annulée dès que le joueur attaque |
| Mort par Corruption 100 | mort normale (l'ennemi « Possédé » du GDD n'est pas codé) |
| Sauvegarde d'un joueur mort | `RespawnAtSafePoint` (défaut, à valider) : enregistré vivant, PV pleins, au point lié s'il est valide, sinon au dernier point sûr ; aucun sac n'est créé par la sauvegarde |

### 9.3 Signatures figées
```cpp
// EMBInteractable.h (Dev A) — interface native
UINTERFACE(MinimalAPI, meta=(CannotImplementInterfaceInBlueprint)) class UEMBInteractable : public UInterface { GENERATED_BODY() };
class EMBERFALL_API IEMBInteractable { GENERATED_BODY() public:
    virtual bool CanInteract(const APawn* Instigator) const = 0;          // serveur et client (pour l'invite d'interface)
    virtual void Interact(APawn* Instigator) = 0;                          // serveur uniquement
    virtual FText GetInteractionText(const APawn* Instigator) const = 0;  // « Ramasser le sac », « Se lier au lit »…
    virtual float GetInteractionHoldTime() const { return 0.f; }           // 4,0 pour la réanimation
};
// AEMBPlayerCharacter (Dev A) : Interact (input) -> trace/overlap 250 cm -> Server_Interact(AActor* Target) ; implémente IEMBInteractable (réanimation)
void RequestRespawn();                                     // client -> serveur, refusé avant 3,0 s
bool IsDeadAwaitingRespawn() const;
// UEMBStatsComponent (Dev A) : ajouts
void ReviveWithFraction(float HealthFraction, float StaminaFraction);   // serveur ; sort de l'état mort
void SetSurvivalModifiers(float MaxHealthMultiplier, float MaxStaminaMultiplier, float StaminaRegenMultiplier); // serveur ; poussé par la survie
// UEMBCombatComponent (Dev A) : ajouts
void ResetFromDeath();                                     // serveur ; Dead -> Idle
void GrantSpawnProtection(float Seconds);                  // serveur ; annulée à la première attaque
void SetExtraFireResistance(float Bonus);                  // serveur ; additionnée au passif FireResistance (plafond 0,9)
// FEMBHitData (Dev A) : ajout `float CorruptionAmount = 0.f;` (Corruption ajoutée à la cible, indépendante du type de dégâts)
// AEMBGameMode (Dev A) : void RespawnPlayer(AController* Controller);   // serveur : DropDeathBag, détruit l'ancien pawn, RestartPlayerAtTransform

// UEMBRespawnSubsystem : UWorldSubsystem (Dev B, serveur)
void BindRespawnPoint(APlayerState* Player, AEMBBuildablePiece* Piece);
bool GetRespawnTransform(AController* Controller, FTransform& OutTransform) const;   // false -> PlayerStart
void DropDeathBag(AController* Controller, const FVector& DeathLocation);            // vide l'inventaire du pawn mort dans un sac
// AEMBDeathBag (Dev B) : acteur répliqué, IEMBInteractable, propriétaire = identifiant stable
// AEMBBuildablePiece (Dev B) : IEMBInteractable si la ligne a bIsRespawnPoint (« Se lier ») ; le lit (« Dormir ») réduit aussi la Corruption de 50 si abrité (trace vers le haut touchant une pièce)
```

## 10. Survie (v1.3)

### 10.1 Arbitrage
Composant dédié `UEMBSurvivalComponent` sur le personnage (pas dans `UEMBStatsComponent`, déjà chargé) ; il **pousse** ses effets vers les stats et le combat par les setters du §9.3. Environnement : `UEMBEnvironmentSubsystem : UWorldSubsystem` (température ambiante, nuit, taux de Corruption, sources de chaleur et purificateurs), alimenté par des `AEMBEnvironmentVolume` (biomes) et par des setters/délégués qu'un futur système météo et jour/nuit appellera ; **aucun système météo n'est codé**.

### 10.2 Constantes (GDD §4.4)
| Jauge | Plage / départ | Évolution | Effets |
|---|---|---|---|
| Faim | 0-100, départ 100 | −1,6/min réelle ; ×1,5 si température ressentie < 5 °C ; × multiplicateur passif `HungerThirstRate` | < 25 : régénération d'endurance ×0,7 ; 0 : −1 PV/s |
| Soif | 0-100, départ 100 | −2,5/min ; ×2 si ressentie > 35 °C ; × `HungerThirstRate` | < 25 : endurance max ×0,8 ; 0 : −2 PV/s |
| Température ressentie | °C | ambiante (biome + nuit −8 °C + météo) + isolation (0 en v1, setter prévu pour l'équipement) + chaleur des feux (+15 °C dans 6 m, non cumulable) ; lissée vers la cible à 1 °C/s | confort 10-28 °C élargi de `ComfortRange` des deux côtés ; < 5 : faim ×1,5 ; < −10 : −1 PV/s ; > 35 : soif ×2 et régénération d'endurance ×0,8 ; > 45 : −1 PV/s |
| Corruption | 0-100, départ 0 | +taux de zone (brume corrompue 0,5/s) ×2 la nuit ; coup reçu : `CorruptionAmount`, sinon dégâts de type Corruption → `clamp(0,2 × dégâts, 2, 20)` ; × `CorruptionGain` ; purificateur −1/s dans 8 m | ≥ 25 : tag `State.Survival.Whispers` (son) ; ≥ 50 : PV max ×0,85 ; ≥ 75 : PV max ×0,70 ; 100 : mort |
| Résistance au feu | 0-0,9 | passif `FireResistance` + bonus temporaires (huile) | dégâts de feu × (1 − résistance) |

Tick du composant : 1 Hz côté serveur (jauges lentes) ; dégâts de survie appliqués en `bDamageOverTime`. Réplication : jauges en `COND_OwnerOnly` (Corruption aussi à tous si ≥ 50 pour les effets visuels).
Régénérations multiplicatives : les malus se multiplient entre eux (faim < 25 et chaleur > 35 → ×0,7 × 0,8).

### 10.3 Consommables (`Data/DT_Consumables.csv`, ressources de l'inventaire)
| Id | Effet | Poids |
|---|---|---|
| `Berries` | faim +10, soif +5 | 0,1 |
| `RawMeat` | faim +10 ; 25 % de chance d'intoxication : −1 PV/s pendant 20 s | 0,5 |
| `CookedMeat` | faim +40 | 0,5 |
| `Stew` | faim +60, soif +10 | 0,8 |
| `BoiledWater` | soif +40 | 0,5 |
| `HealingFlask` | PV +40 % du maximum | 0,3 |
| `Antidote` | annule l'intoxication | 0,2 |
| `CorruptionTonic` | Corruption −30 | 0,3 |
| `FireResistOil` | résistance au feu +0,25 pendant 120 s (non cumulable, rafraîchit) | 0,3 |
Délai entre deux consommations : 1,0 s. Consommation : `Server_Consume(FName)` (Reliable, validé, plafond 5/s).

### 10.4 Signatures figées
```cpp
// UEMBSurvivalComponent (Dev D)
float GetHunger() const; float GetThirst() const; float GetCorruption() const; float GetFeltTemperature() const;
void RequestConsume(FName ConsumableId);                    // client -> Server_Consume
void AddCorruption(float Amount);  void ReduceCorruption(float Amount);   // serveur
void ApplyRespawnValues();                                   // serveur : §9.2
void SetPassiveModifiers(const FEMBPassiveModifiers& Mods);  // poussé par la progression (HungerThirstRate, ComfortRange, CorruptionGain, FireResistance)
void SetInsulation(float DegreesC);                          // pour l'équipement futur
void ExportSaveData(FEMBSurvivalSaveData& Out) const; void ImportSaveData(const FEMBSurvivalSaveData& In);
// FEMBSurvivalSaveData (EMBSurvivalTypes.h, champs SaveGame) : Hunger, Thirst, Corruption, FeltTemperature, FireOilRemaining, IntoxicationRemaining
// UEMBEnvironmentSubsystem (Dev D)
float GetAmbientTemperatureAt(const FVector& Location) const; float GetCorruptionRateAt(const FVector& Location) const; bool IsNight() const;
float GetHeatBonusAt(const FVector& Location) const; float GetPurificationRateAt(const FVector& Location) const;
void SetTimeOfDay(float Hours);  void SetWeatherTemperatureOffset(float DeltaC);   // appelés par un futur système météo / jour-nuit
void RegisterHeatSource(AActor* Source, float Radius, float BonusC);  void UnregisterHeatSource(AActor* Source);
void RegisterPurifier(AActor* Source, float Radius, float RatePerSecond);  void UnregisterPurifier(AActor* Source);
// délégué OnEnvironmentChanged()
```
`AEMBProgressionComponent::ApplyModifiersToPawn` pousse aussi vers `UEMBSurvivalComponent::SetPassiveModifiers` (Dev A). `FEMBBuildPieceRow` reçoit en fin : `bool bIsRespawnPoint`, `float HeatRadius`, `float HeatBonus`, `float PurifyRadius`, `float PurifyRate` (feu de camp : respawn, 600 cm, +15 °C ; forge : 400 cm, +10 °C ; lit : respawn ; nouvelle ligne `PurifyingBrazier` : 800 cm, 1,0/s, `RequiredGlyph = G_PurifyingBrazier`, coût 20 Stone + 5 IronIngot + 2 MoonSilver).

## 11. Boss 1 et 2 (v1.3)

### 11.1 Base commune
`AEMBBossBase : ACharacter, IEMBHitReceiver` (Dev C) factorise : stats et posture, mise à l'échelle coop, phases par seuils de PV avec invulnérabilité de transition, enragé optionnel, points faibles par os, attaques chronométrées (`BeginTimedAttack`), recharges, historique anti-répétition, dégâts de zone/cône, rencontre (`IsEncounterActive`), défaite (`GetDefeatedEventTag`, XP depuis `UEMBProgressionSettings`, `NotifyWorldEvent`, `ApplyDefeatedFromSave`). `AEMBDragonBoss` en hérite sans changer son comportement ni ses valeurs. `UBTTask_EMBSelectAttack`, le service et le décorateur de phase travaillent sur `AEMBBossBase`. La sauvegarde itère sur `AEMBBossBase`.

**Rencontre** (v1.3 après revue) : `ArenaRadius` par boss (dragon 6 000 cm, Colosse 3 000, Nyxariel 4 000). `IsEncounterActive` = rencontre engagée, boss vivant et au moins un joueur vivant dans `ArenaRadius`. Après **30 s sans joueur vivant dans l'arène**, la rencontre est abandonnée : PV remis au maximum, attaques et étourdissement interrompus, **phase conservée** (l'arène est irréversible), échelle coop recalculée à la rencontre suivante. La sauvegarde reportée pendant une rencontre est forcée au-delà de 300 s de report. Constante partagée `EMBBossDefaults::ArenaBossSearchInterval`.

### 11.2 Colosse d'Ossivaire (`AEMBColossusBoss`)
PV 12 000 ; posture 600, régénération 25/s après 3 s, étourdi 4 s ; résistances physique 10 %, feu 40 %, foudre −20 %, corruption 0 % ; point faible `core` dans le dos ×1,5, **exposé 3 s après le Séisme** (sinon ×1,0) ; P2 à 50 % (invulnérable 4 s, télégraphes ×0,9) ; marche 350 cm/s ; XP `GuardianXP`, tag `Event.Boss.Killed.Guardian`.

| Attaque (`Attack.Guardian.*`) | Phases | Portée | Angle | Télégraphe | Esquive | Dégâts / poise | Garde / parade | Poids | Recharge |
|---|---|---|---|---|---|---|---|---|---|
| HammerFist | 1-2 | 0-700 | ±45° | 1,0 | 0,90-1,15 | 180 / 50 | blocable / non parable | 30 | 3 |
| ArmSweep | 1-2 | 0-900 | ±90° | 0,8 | 0,70-0,95 | 140 / 40 | blocable | 25 | 4 |
| LionBite | 1-2 | 0-500 | ±30° | 0,6 | 0,55-0,75 | 120 / 30 | **parable** | 25 | 3 |
| SerpentLash | 1-2 | 0-1 000 | arrière > 110° | 0,7 | 0,60-0,85 | 160 / 60 | blocable | 35 | 5 |
| RamCharge | 1-2 | 800-2 500 | ±20° | 1,4 | 1,30-1,60 (latéral) | 220 / 90 | imblocable | 20 | 10 ; percute un obélisque ou un mur → étourdi 3 s |
| Quake | 1-2 | 0-800 | 360° | 1,8 | 1,75-2,00 (onde rayon 800) | 200 / 80 | imblocable | 15 | 12 ; noyau exposé 3 s |
| RuneWaves | 2 | 0-2 500 | 360° | 1,0 | 3 anneaux à 0,8 s d'intervalle, 1 000 cm/s, épaisseur 150 | 90 chacun | imblocable | 20 | 14 |
| LionRoar | 2 | 0-1 200 | ±30° | 0,5 | sortir du cône | 0 / 80 | — | 15 | 12 ; enchaîne HammerFist |
| Resonance | 2 | — | — | canalisation 3,0 s | interrompue par ≥ 150 de posture | soin 3 % PV max par obélisque intact | — | 10 (0 si aucun obélisque) | 25 |
Arène `AEMBColossusArena` : 4 obélisques de 500 PV (endommageables par les joueurs), allumés en P2.

### 11.3 Nyxariel, l'Aile Flétrie (`AEMBSeraphBoss`)
PV 24 000 ; posture 700, régénération 50/s après 2,5 s, étourdi 3 s ; résistances physique 0 %, feu −25 %, corruption 60 % ; P2 à 60 %, P3 à 25 % (au sol, vitesse ×1,2) ; transitions invulnérables 3 s ; vol 1 800 cm/s ; XP `SeraphXP`, tag `Event.Boss.Killed.Seraph`.

| Attaque (`Attack.Seraph.*`) | Phases | Portée | Télégraphe | Esquive | Dégâts / poise | Parade | Poids | Recharge |
|---|---|---|---|---|---|---|---|---|
| Thrust | 1-3 | 600-1 800 (±20°) | 0,5 | 0,45-0,60 (dash 1 800 cm en 0,25 s) | 160 / 50 | **Oui** | 30 | 3 |
| Feint | 1-3 | 600-1 800 | 1,0 + délai aléatoire 0,3-0,9 | au flash de la pointe | 160 / 50 | Oui | 15 | 6 |
| WingScissors | 1-3 | 0-500 (±60°) | 0,4 | coups à 0,40 et 0,75 | 120 ×2 / 30 | Non | 25 | 4 |
| FeatherRain | 1-2 | 500-3 000 (±40°) | 0,9 | 24 projectiles à 3 000 cm/s | 25 + 5 Corruption chacun | Non | 15 | 10 |
| LanceThrow | 1-3 | 1 000-4 000 (±15°) | 0,7 | projectile 4 000 cm/s | 200 / 60 | parade parfaite = renvoi, 300 dégâts au boss | 20 | 8 |
| SpiralDive | 1-2 | 800-4 000 | 1,2 (ombre) | flash 0,35 s avant impact, rayon 400 | 200 / 70 | Non | 15 | 12 |
| TwinShadows | 2 | — | 1,5 | 2 clones (1 coup les dissipe, 20 s), attaques répétées avec 0,3 s de retard à 30 % des dégâts | — | — | 10 | 30 |
| BlackHalo | 2-3 | 0-600 | 1,5 | sphère rayon 600 | 250 + 20 Corruption | Non | 15 | 15 |
| DeadStarFall | 3 | tout | 0,8 avant chaque plongeon | 3 plongeons espacés de 0,9 s, rayon 400 | 220 / 80 chacun | Non | 20 | 20 ; **vulnérable 4 s** ensuite |
Arène `AEMBSeraphArena` : 3 cloches ; frapper une cloche quand Nyxariel vole l'étourdit 2 s (une fois par cloche).
