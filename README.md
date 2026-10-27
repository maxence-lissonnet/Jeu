# Emberfall — Dossier de conception

> Jeu vidéo 3D de survie / action-RPG à la troisième personne, dark fantasy médiévale réaliste (références : Elden Ring, ARK, Valheim).
> Nom de code « Emberfall » (modifiable). Dossier produit par l'équipe ADWEB le 23/10/2026.
> **Le code C++ n'a jamais été compilé ni exécuté** (Unreal Engine n'est pas installé sur le poste de production du dossier). Voir « Vérifié / non vérifié ».

## Sommaire

| Document | Contenu |
|---|---|
| [`docs/00_Constantes_et_Contrats.md`](docs/00_Constantes_et_Contrats.md) | **Source unique de vérité** : toutes les valeurs de gameplay (combat, construction, dragon) et les signatures C++ partagées. Le GDD et le code en dérivent. |
| [`docs/01_GDD.md`](docs/01_GDD.md) | Game Design Document : direction artistique, boucles, survie, construction, combat, bestiaire, progression (Skill Tree, Knowledge Tree, Glyphes), 3 boss détaillés, coop. |
| [`docs/02_Architecture.md`](docs/02_Architecture.md) | Architecture du code : diagrammes Mermaid (classes, FSM de combat, séquences), délégués et Gameplay Tags, Behavior Tree du dragon, progression, sauvegarde, réplication. |
| [`docs/03_Scalabilite.md`](docs/03_Scalabilite.md) | Plan de performance : budgets cibles par plateforme (à valider par profilage), Nanite, LOD/HLOD, World Partition, Lumen, VSM, textures, Asset Manager, PSO, pools, Significance Manager, réseau. |
| `Source/Emberfall/` | Module C++ UE5 : construction, combat (FSM), stats, personnage, IA du dragon final, progression (XP, glyphes, Skill Tree), sauvegarde (v1.2). |
| `Data/*.csv` | DataTables importables : attaques du dragon, glyphes, nœuds du Skill Tree, pièces de construction. |
| `Config/` | Configuration projet (GameMode par défaut, canaux de collision, Enhanced Input, réglages de progression et de sauvegarde, tags des glyphes et des compétences). |

## Choix du moteur : Unreal Engine 5.4+ en C++

1. Les exigences visuelles (illumination globale dynamique, géométrie massive, PBR 4K, météo et cycle jour/nuit complets) correspondent exactement à **Lumen, Nanite, Virtual Shadow Maps et Virtual Textures**, intégrés et éprouvés en production.
2. Un moteur web (Three.js / WebGL / WebGPU) n'offre ni GI dynamique de ce niveau, ni streaming de monde ouvert, ni physique de destruction intégrée : il faudrait tout réécrire, pour un résultat inférieur.
3. UE5 fournit nativement ce que le jeu demande : **Chaos** (arbres, rochers, effondrement), **Niagara** (souffles, cendres), **Behavior Trees / EQS** (IA des boss), **World Partition** (monde ouvert), **réplication serveur-autoritaire** (coop 1-4), **Enhanced Input** et **Gameplay Tags**.
4. Le **C++** garantit le contrôle des performances sur les systèmes critiques (intégrité structurelle, FSM de combat, IA) ; les Blueprints restent réservés au contenu (montages, VFX, mise en scène).
5. Le genre visé (Elden Ring, ARK : Survival Ascended) a des références directes sur UE, ce qui facilite recrutement, outillage et portage consoles.

## Arborescence

```
EmberfallGDD/
├── README.md
├── Emberfall.uproject
├── Config/
│   ├── DefaultEngine.ini          (Enhanced Input, canaux EMBMeleeHit / EMBBuilding)
│   ├── DefaultGame.ini
│   ├── Tags/EmberfallGlyphs.ini   (tags Glyph.*)
│   └── Tags/EmberfallSkills.ini   (tags Skill.*)
├── Data/
│   ├── DT_DragonAttacks.csv       (FEMBDragonAttackRow, 11 attaques)
│   ├── DT_ColossusAttacks.csv     (même struct, 9 attaques du Colosse)
│   ├── DT_SeraphAttacks.csv       (même struct, 9 attaques de Nyxariel)
│   ├── DT_Consumables.csv         (FEMBConsumableRow, 9 consommables)
│   ├── DT_ResourceNodes.csv       (FEMBResourceNodeRow, 14 nœuds de récolte)
│   ├── DT_Items.csv               (FEMBItemRow, 35 objets durables)
│   ├── DT_Recipes.csv             (FEMBRecipeRow, 79 recettes dont 35 réparations)
│   ├── DT_Weather.csv             (FEMBWeatherRow, 6 biomes)
│   ├── DT_Creatures.csv           (FEMBCreatureRow, 3 archétypes)
│   ├── DT_GhoulAttacks.csv, DT_RockWyvernAttacks.csv, DT_SylvadracAttacks.csv (FEMBDragonAttackRow)
│   ├── DT_Glyphs.csv              (FEMBGlyphRow, 36 glyphes dont 6 secrets)
│   ├── DT_SkillNodes.csv          (FEMBSkillNodeRow, 20 nœuds)
│   └── DT_BuildPieces.csv         (FEMBBuildPieceRow)
├── docs/
│   ├── 00_Constantes_et_Contrats.md
│   ├── 01_GDD.md
│   ├── 02_Architecture.md
│   └── 03_Scalabilite.md
└── Source/
    ├── Emberfall.Target.cs
    ├── EmberfallEditor.Target.cs
    ├── EmberfallServer.Target.cs  (serveur dédié, nécessite un moteur compilé depuis les sources)
    └── Emberfall/
        ├── Emberfall.Build.cs
        ├── Public/   (en-têtes .h)
        └── Private/  (implémentations .cpp)
```

Fichiers du module, par système :

| Système | Classes |
|---|---|
| Noyau | `Emberfall` (module, `LogEmberfall`), `EMBGameplayTags` (tags natifs `EMBTags::*`) |
| Personnage | `AEMBPlayerCharacter`, `UEMBCharacterMovementComponent` (sprint prédit) |
| Stats | `UEMBStatsComponent` (PV, endurance, poise/posture, répliqué) |
| Combat | `EMBCombatTypes.h`, `IEMBHitReceiver`, `UEMBCombatComponent` (FSM), `UEMBAnimNotifyState_ComboWindow / _IFrames / _HitWindow` |
| Construction | `EMBBuildingTypes.h`, `UEMBBuildingComponent`, `AEMBBuildablePiece`, `UEMBStructureSubsystem` (intégrité, effondrement), `UEMBInventoryComponent` |
| Partie et joueurs | `AEMBGameMode`, `AEMBPlayerState`, `AEMBDiscoveryVolume` (lieux découverts) |
| Progression | `EMBProgressionTypes.h`, `UEMBProgressionSettings` (réglages `DefaultGame.ini`), `UEMBProgressionComponent` (sur le PlayerState, répliqué), `UEMBProgressionSubsystem` (règles, DataTables, événements monde) |
| Sauvegarde | `EMBSaveTypes.h`, `UEMBSaveGame` (format versionné), `UEMBSaveSubsystem` (GameInstance : slots A/B, autosave 300 s, restauration des joueurs, reconstruction des bases) |
| Mort et réapparition (v1.3) | `IEMBInteractable`, `AEMBDeathBag` (sac), `UEMBRespawnSubsystem` (lits / feux de camp, liaisons), réanimation par un allié dans `AEMBPlayerCharacter`, `AEMBGameMode::RespawnPlayer` |
| Survie (v1.3) | `EMBSurvivalTypes.h`, `UEMBSurvivalComponent` (faim, soif, température, Corruption, consommables), `UEMBEnvironmentSubsystem` (température, nuit, chaleur, purification — à alimenter par un futur système météo / jour-nuit), `AEMBEnvironmentVolume` (biomes) |
| Boss (base commune, v1.3) | `AEMBBossBase`, `AEMBBossAIController`, `EMBBossTags`, `UBTTask_EMBSelectAttack`, `UBTService_EMBUpdateCombatContext`, `UBTDecorator_EMBDragonPhase`, `EMBDragonTypes.h` (`FEMBBossAttackRow`) |
| Boss 1 — Colosse d'Ossivaire | `AEMBColossusBoss`, `AEMBColossusArena` + `AEMBObelisk`, `UBTTask_EMBColossusCharge` |
| Boss 2 — Nyxariel | `AEMBSeraphBoss`, `AEMBSeraphShadow` (clones), `AEMBSeraphArena` + `AEMBSeraphBell`, `AEMBProjectile` (poolé), `UBTTask_EMBSeraphDash`, `UBTTask_EMBSeraphPlunge`, `UBTTask_EMBSeraphReposition` |
| Boss final — Vaelthyrax | `AEMBDragonBoss`, `AEMBDragonAIController`, `AEMBDragonArena`, `UBTTask_EMBDragonBreath`, `UBTTask_EMBDragonDiveAttack` |
| Récolte (v1.4) | `EMBResourceNodeTypes.h`, `AEMBResourceNode` (nœuds frappés à l’outil), `AEMBFallingTree` (chute physique), `UEMBResourceNodeSubsystem` (réapparition, carcasses, sauvegarde) |
| Objets, équipement, artisanat (v1.4) | `EMBItemTypes.h`, `UEMBEquipmentComponent` (sac de 40 objets, 6 emplacements, durabilité, stats poussées au combat), `UEMBCraftingComponent` (file serveur, ateliers construits, glyphes) |
| Météo et jour/nuit (v1.4) | `EMBWeatherTypes.h`, `UEMBWeatherSubsystem` (horloge 48 min, météo par biome), `AEMBWorldStateReplicator` (réplication), `AEMBSkyController` (soleil, lune, brouillard, nuages ; acteurs du niveau) |
| Bestiaire (v1.4) | `AEMBCreatureBase` (+ goule, wyverneau), `AEMBCreatureAIController`, tâches/service `BTTask_EMBCreatureAttack`, `BTTask_EMBCreatureFlee`, `BTService_EMBCreatureContext`, `AEMBTerritorialDragon` (Sylvadrac, dérive de `AEMBBossBase`), `AEMBCreatureSpawner`, `UEMBCreatureSignificance` |

## Intégrer le module dans un projet UE5

**Option A — projet fourni.** Ouvrir `Emberfall.uproject` avec Unreal Engine 5.4 (clic droit → *Generate Visual Studio project files*), compiler la cible `EmberfallEditor` (Development Editor, Win64).

**Option B — projet existant.**
1. Copier `Source/Emberfall/` dans le dossier `Source/` du projet.
2. Déclarer le module dans le `.uproject` :
   ```json
   "Modules": [ { "Name": "Emberfall", "Type": "Runtime", "LoadingPhase": "Default" } ],
   "Plugins": [ { "Name": "EnhancedInput", "Enabled": true } ]
   ```
3. Ajouter `"Emberfall"` à `ExtraModuleNames` dans les fichiers `*.Target.cs`.
4. Dépendances déjà déclarées dans `Emberfall.Build.cs` : `Core`, `CoreUObject`, `Engine`, `InputCore`, **`EnhancedInput`**, **`GameplayTags`**, **`AIModule`**, **`NavigationSystem`**, **`Niagara`**, **`UMG`**, `NetCore`, `GameplayTasks` (Behavior Trees), `Slate`, `SlateCore`. Les débris Chaos passent par un acteur Blueprint (Geometry Collection) : ajouter `Chaos` et `GeometryCollectionEngine` seulement si du C++ doit les manipuler directement.
5. Reprendre dans `Config/DefaultEngine.ini` : classe d'input Enhanced Input, canaux `EMBMeleeHit` (`ECC_GameTraceChannel1`), `EMBBuilding` (`ECC_GameTraceChannel2`) et `EMBProjectile` (`ECC_GameTraceChannel3`) — si le projet utilise déjà ces canaux, les renuméroter et ajuster les valeurs par défaut dans `UEMBCombatComponent` et `UEMBBuildingComponent`.
6. Copier `Config/Tags/EmberfallGlyphs.ini` et `EmberfallSkills.ini` dans `Config/Tags/`, et reprendre dans `DefaultGame.ini` les sections `EMBProgressionSettings` et `EMBSaveSubsystem` (chemins des DataTables sous `/Game/Data/`) et `DirectoriesToAlwaysCook`, dans `DefaultEngine.ini` le `GlobalDefaultGameMode`.
7. Régénérer les fichiers de projet et compiler.

**Après la compilation (dans l'éditeur)** :
- Importer les CSV de `Data/` en DataTables avec les structs `FEMBDragonAttackRow` (aussi pour `DT_ColossusAttacks` et `DT_SeraphAttacks`), `FEMBGlyphRow`, `FEMBSkillNodeRow`, `FEMBBuildPieceRow`, `FEMBConsumableRow`, `FEMBResourceNodeRow`, `FEMBItemRow`, `FEMBRecipeRow`, `FEMBWeatherRow`, `FEMBCreatureRow` (les tables d’attaques des créatures en `FEMBDragonAttackRow`), et les enregistrer sous `/Game/Data/` sous le nom du fichier : la progression, la survie et la sauvegarde les chargent par ces chemins.
- Créer les Behavior Trees et Blackboards du Colosse et de Nyxariel (structure en commentaire d'en-tête de `EMBColossusBoss.h` et `EMBSeraphBoss.h`), les Blueprints des boss, du sac, des projectiles, clones, cloches et obélisques ; les Input Actions `IA_Interact`, `IA_Respawn` (optionnelle), `IA_QuickConsume` (optionnelle).
- v1.4 : Behavior Trees `BT_Creature`/`BB_Creature` et `BT_Sylvadrac` (structure dans les en-têtes), Blueprints de nœuds de récolte (mesh avec collision Overlap sur `EMBMeleeHit`, `NodeId` généré), carcasses (colonne `NodeClass`), créatures et spawners (NavMesh requis), un `AEMBSkyController` dans le niveau et un `BiomeId` sur chaque volume d’environnement ; vérifier à la première ouverture que le `SignificanceManager` est bien créé (`DefaultEngine.ini`, clé écrite de mémoire).
- Créer les Blueprints enfants (`BP_PlayerCharacter`, `BP_Vaelthyrax`, `BP_DragonAIController`, pièces de construction), assigner montages, Input Actions, Mapping Context, matériau fantôme (paramètre `GhostColor`), DataTables.
- Placer les AnimNotifyStates (`ComboWindow`, `HitWindow`) dans les montages d'attaque ; les i-frames de roulade sont pilotées par le code (le notify `IFrames` est optionnel).
- Créer le Blackboard et le Behavior Tree du dragon selon `docs/02_Architecture.md` §4.
- Physics Asset du dragon et des personnages en *Overlap* sur `EMBMeleeHit` (points faibles par os).

## Vérifié / non vérifié

| Élément | Statut |
|---|---|
| Compilation C++ (UHT, UBT) | **Non vérifiée** : aucun build n'a été lancé, Unreal n'est pas installé. Attendez-vous à des corrections au premier build. |
| Exécution, tests de gameplay, tests réseau | **Non réalisés.** |
| Revue de code statique | v1.1 : trois relectures par système, corrections, revue d’intégration. v1.2 (progression, sauvegarde) et v1.3 (mort, survie, boss) : à chaque fois une revue par système, une revue d’intégration, puis une passe de corrections groupée. Aucune erreur de compilation certaine relevée, mais les API ont été vérifiées **de mémoire**, pas contre les sources du moteur. |
| Fonctionnalités prévues mais non codées (après v1.4) | Exécution des compétences actives, UI, raids des dragons territoriaux sur les bases, autres créatures et dragons du GDD (Loup-fléau, Glacivorn, Tempestar…), effets de la météo sur le gameplay (feux éteints sous la pluie, neige qui ralentit), ressources « peau » et « fourrure » (les armures de peaux/fourrures utilisent Fibre et Viande crue en attendant), réparation à moitié prix du glyphe `S_SunkenCodex`, ennemi « Possédé ». |
| Cohérence des valeurs GDD ↔ code ↔ CSV | Contrôlée par le chef d'équipe et les relecteurs contre `00_Constantes_et_Contrats.md`. |
| Import des CSV dans l'éditeur | **Non testé** ; structure des colonnes vérifiée par lecture (PowerShell `Import-Csv`). |
| Diagrammes Mermaid | Non rendus dans un outil. |
| Budgets de performance (`03_Scalabilite.md`) | **Cibles** à valider par profilage (Unreal Insights) ; aucune mesure. |
| Assets (meshes, animations, sons, VFX) | Inexistants : le dossier ne contient que du code, des données et de la documentation. |

## Progression et sauvegarde (v1.2) — réglages à valider

| Réglage (`Config/DefaultGame.ini`) | Valeur codée par défaut | Alternative |
|---|---|---|
| `SecretGlyphMode` | `RevealOnly` : un déclencheur secret (boss vaincu, lieu découvert) **révèle** le glyphe, qui reste à acheter | `AutoUnlock` : débloqué gratuitement si niveau, parents et savoirs sont remplis |
| `DeathSaveBehavior` | `RespawnAtSafePoint` : un joueur mort au moment de la sauvegarde est enregistré PV pleins au point de réapparition lié, sinon au dernier point sûr ; aucun sac n’est créé (quitter en étant mort évite donc la perte du sac) | `KeepPosition` |
| Sauvegarde illisible (version plus récente que le jeu) | aucune écriture pendant la session (le fichier n'est pas écrasé), erreur dans le journal | refuser de démarrer le monde |

Identifiant de joueur sauvegardé : identifiant Steam/EOS s'il existe ; sinon `LocalHost` pour l'hôte et `Guest_<nom>` pour un client (non garanti unique ; un second joueur connecté avec le même identifiant démarre un personnage neuf non sauvegardé).

## Fichiers de travail
Les copies `*.bak-20261023*` (sources, données, configuration, docs) sont les sauvegardes faites avant chaque modification, et `Data/DT_SkillNodes.csv.tmp` est un fichier intermédiaire du chef d'équipe : rien n'a été supprimé, à archiver ou retirer par vous après validation (Unreal ignore ces extensions).
