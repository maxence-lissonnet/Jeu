# Emberfall — Plan de performance et de scalabilité

> **Statut :** v1, proposition d'architecture technique, à valider par l'équipe moteur.
> **Règle de lecture :** **tous les chiffres de ce document sont des CIBLES de conception**, pas des mesures. Chacun doit être confirmé ou corrigé par profilage sur matériel réel (Unreal Insights, `stat unit`, `stat gpu`, `ProfileGPU`, Memory Insights) au fil des jalons. Un chiffre qui n'a pas été mesuré au moins une fois sur la plateforme visée est marqué « non mesuré » dans les rapports de jalon.
> **Moteur :** Unreal Engine 5.4+. Quand une fonctionnalité dépend de la version, c'est précisé (« 5.4 » / « plus récent ») ; en cas de doute, la fonctionnalité est traitée comme **non acquise** tant qu'elle n'a pas été testée dans la version retenue.
> **Références croisées :** constantes de construction et d'effondrement dans `00_Constantes_et_Contrats.md` §5 (notamment `MaxCollapsePerFrame` = 32, `CollapseWaveDelay` = 0,08 s) ; boss dragon §6.

---

## 0. Synthèse des décisions

| # | Décision | Pourquoi |
|---|---|---|
| D1 | **Plateforme de référence = PS5 / Xbox Series X en mode Performance (60 fps)**. Le PC moyen s'aligne dessus ; Series S est la contrainte basse, traitée dès le jalon Vertical Slice. | Concevoir sur la machine la plus puissante puis « dégrader » est la cause n° 1 des portages ratés. |
| D2 | **Nanite pour tout le statique opaque** (roches, falaises, architecture, pièces de construction, troncs). Végétation fine (feuillage, herbe) : stratégie séparée (§3.3). | Nanite supprime l'essentiel du travail de LOD et des draw calls sur l'opaque statique ; il reste coûteux sur le masqué + WPO. |
| D3 | **Lumen logiciel par défaut sur consoles et PC moyen**, Lumen matériel (HWRT) en option sur PC haut. Series S : Lumen logiciel au plus bas, avec repli SSGI + DFAO à évaluer au jalon Vertical Slice. | Cycle jour/nuit dynamique = pas d'éclairage précalculé possible ; Lumen logiciel est le meilleur rapport coût/qualité sur consoles. |
| D4 | **World Partition + One File Per Actor + HLOD**, plusieurs grilles de streaming, Data Layers pour biomes, événements et états d'arène. | Monde ouvert, équipe multiple, état du monde dynamique. |
| D5 | **Pièces de construction : rendu en instances (ISM Nanite par base et par tronçon, §14), état répliqué par un acteur « Base » unique (Fast Array)**, pas un acteur répliqué par pièce. À trancher en ADR avec Dev B (§13.4). | Des milliers d'acteurs répliqués coûtent cher en CPU serveur et en mémoire, même dormants. |
| D6 | **Effondrement : intégrité calculée sur le serveur en incrémental (sous-graphe dépendant seulement), destructions étalées par vagues ; débris purement cosmétiques, locaux à chaque client, tirés d'un pool.** | Respecte `MaxCollapsePerFrame` = 32 par vague ; zéro débris répliqué. État du code (v1.1) : le recalcul d'intégrité se fait en une passe, seules les destructions sont étalées (§ 14.3). |
| D7 | **Réseau : pilote de réplication standard + push model + dormance + NetCullDistance.** Replication Graph ou Iris seulement si le profilage serveur le justifie. | 4 joueurs maximum : la complexité de ReplicationGraph/Iris ne se justifie qu'en cas de problème mesuré. |
| D8 | **Aucun Spawn/Destroy à haute fréquence en combat** : pools pour projectiles, flèches, zones de feu, débris, systèmes Niagara. | Évite les pics de game thread et la pression sur le ramasse-miettes (GC). |
| D9 | **PSO : bundled PSO cache + PSO precaching**, collecte automatisée sur chaque build de jalon. | La saccade de compilation de shaders est le défaut perçu le plus pénalisant d'UE5 sur PC. |

---

## 1. Plateformes cibles et budgets

### 1.1 Profils de plateforme (hypothèses)

| Profil | Matériel de référence (hypothèse à figer avec la production) | Mode | Fps cible | Sortie | Résolution interne cible | Upscaler |
|---|---|---|---|---|---|---|
| **PC haut** | GPU classe RTX 4070 / RX 7800 XT, 12 Go VRAM ; CPU 8 cœurs récent ; 32 Go RAM ; SSD NVMe | Épique | 60 (non verrouillé, cible 60-90) | 1440p | 960p-1080p (≈ 67-75 %) | DLSS / FSR / XeSS / TSR au choix |
| **PC moyen (= config recommandée)** | GPU classe RTX 3060 / RX 6600 XT, 8 Go VRAM ; CPU 6 cœurs ; 16 Go RAM ; SSD | Élevé | 60 | 1080p | 720p-810p (≈ 67-75 %) | DLSS / FSR / TSR |
| **PC minimum** | GPU DX12 SM6 classe RTX 2060 / RX 5600 XT, 6 Go VRAM ; 16 Go RAM ; **SSD obligatoire** | Bas | 30 (stable) | 1080p | 540p-720p | TSR / FSR / DLSS |
| **PS5 / Xbox Series X — Performance** | Kit de dev | Console « Perf » | **60** | 1440p-4K | ≈ 864p-1080p, résolution dynamique | TSR |
| **PS5 / Xbox Series X — Qualité** | Kit de dev | Console « Qualité » | **30** (option 40 Hz sur écrans 120 Hz si budget atteint) | 4K | ≈ 1296p-1440p, résolution dynamique | TSR |
| **Xbox Series S** | Kit de dev | Unique | **30** (60 non promis) | 1080p-1440p | ≈ 648p-900p, résolution dynamique | TSR |

Points d'attention :
- **Nanite et Lumen exigent un GPU DX12 Shader Model 6** sur PC : la configuration minimale doit être choisie en conséquence (pas de GPU « DX11 seulement »).
- **SSD obligatoire** sur PC : le streaming World Partition et le streaming de textures sont conçus pour des latences de SSD.
- Les **consoles utilisent une résolution dynamique** (`r.DynamicRes.OperationMode`) : la résolution interne varie dans une plage, le fps reste fixe.
- La **génération de trames** (DLSS Frame Generation, FSR Frame Generation) est proposée sur PC comme option, jamais comptée dans la cible : elle n'améliore pas la latence et exige une base d'environ 60 fps pour être agréable.

### 1.2 Budget par frame (ms)

Le budget est atteint quand **chaque thread** tient sous le budget de frame avec une marge de 10 % (pics de combat inclus). Mesure : `stat unit` / `stat unitgraph` (Game, Draw, RHIT, GPU) puis Unreal Insights pour le détail.

| Profil | Frame | Game thread | Render thread (Draw + RHI) | GPU | Marge visée |
|---|---|---|---|---|---|
| PC haut 60 fps | 16,6 | ≤ 10 | ≤ 10 | ≤ 15 | 10 % |
| PC moyen 60 fps | 16,6 | ≤ 12 | ≤ 12 | ≤ 15 | 10 % |
| PC min 30 fps | 33,3 | ≤ 20 | ≤ 20 | ≤ 30 | 10 % |
| PS5 / XSX Perf 60 fps | 16,6 | ≤ 12 | ≤ 12 | ≤ 15 | 10 % |
| PS5 / XSX Qualité 30 fps | 33,3 | ≤ 20 | ≤ 20 | ≤ 30 | 10 % |
| Series S 30 fps | 33,3 | ≤ 22 | ≤ 22 | ≤ 30 | 10 % |

> **Listen server :** l'hôte exécute le serveur **et** son client dans le même game thread. Le budget game thread ci-dessus inclut la part serveur (IA, intégrité des bases, réplication). Sur l'hôte, réserver **3 à 4 ms de game thread** au travail serveur (cible). C'est le risque n° 1 sur Series S (§8.6).

### 1.3 Répartition indicative du budget GPU

Cible pour un plan « forêt dense + base + 10 créatures + météo » (scène de référence n° 2, §16.1). Mesure avec `stat gpu` et `ProfileGPU`.

| Poste GPU | PC haut 60 (15 ms) | PS5/XSX Perf 60 (15 ms) | PS5/XSX Qualité 30 (30 ms) | Series S 30 (30 ms) |
|---|---|---|---|---|
| Nanite (visibility buffer, raster, culling) + base pass / matériaux | 3,0 | 3,0 | 6,0 | 6,5 |
| Géométrie non Nanite (personnages, créatures, herbe, terrain) | 1,5 | 1,5 | 3,0 | 3,0 |
| Ombres (VSM ou CSM) | 2,0 | 2,0 | 4,0 | 4,0 |
| Lumen (scène, Final Gather, réflexions) | 4,0 | 3,5 | 7,0 | 5,5 (ou repli §5.5) |
| Ciel, nuages volumétriques, brouillard volumétrique | 1,0 | 1,2 | 2,5 | 2,5 |
| Translucidité + Niagara (feu, fumée, souffle) | 1,0 | 1,0 | 2,0 | 2,5 |
| Post-process (TSR, DOF, flou de mouvement, bloom, tonemap) | 1,5 | 1,8 | 3,0 | 3,0 |
| Divers / marge | 1,0 | 1,0 | 2,5 | 3,0 |

Règle : si un poste dépasse sa cible de plus de 20 % sur la scène de référence, il fait l'objet d'un ticket « perf » bloquant pour le jalon suivant.

### 1.4 Budgets mémoire

Chiffres de plateforme publics à titre de repère ; **la documentation plateforme (sous NDA) fait foi**.
- Xbox Series X : 16 Go, dont environ 13,5 Go disponibles pour le jeu (chiffre public Microsoft).
- PS5 : 16 Go GDDR6 unifiés ; la part disponible pour le jeu est donnée par la documentation Sony.
- Series S : 10 Go, dont environ 8 Go disponibles pour le jeu (ordre de grandeur public) — **c'est la contrainte mémoire de référence**.

| Profil | VRAM / mémoire GPU cible | RAM CPU cible (processus) | Pool de streaming de textures (`r.Streaming.PoolSize`, Mo) — point de départ | Pool VSM (pages physiques) | Pool Virtual Textures |
|---|---|---|---|---|---|
| PC haut | ≤ 10 Go (tient dans 12 Go) | ≤ 12 Go | 3 000-4 000 | défaut | défaut ×1,5 |
| PC moyen | ≤ 7 Go (tient dans 8 Go) | ≤ 10 Go | 2 000 | défaut | défaut |
| PC min | ≤ 5,5 Go (tient dans 6 Go) | ≤ 9 Go | 1 200 | réduit | réduit |
| PS5 / XSX | ≤ 12 Go au total, réparti GPU/CPU | (inclus) | 2 500-3 000 | défaut | défaut |
| Series S | **≤ 7,5 Go au total** | (inclus) | 1 000-1 200 | réduit | réduit |

Répartition indicative de la mémoire totale sur Series S (cible) : textures en streaming 1,2 Go ; meshes (Nanite + non Nanite) 1,0 Go ; render targets et buffers (Lumen, VSM, TSR, GBuffer) 1,2 Go ; audio 0,4 Go ; animations 0,4 Go ; code + UObjects + gameplay 1,5 Go ; Niagara 0,2 Go ; marge 1,5 Go.

Mesure : Memory Insights (`-trace=default,memory`), `memreport -full`, `stat streaming`, `ListTextures`. Sur PC, `r.Streaming.LimitPoolSizeToVRAM 1` pour éviter qu'un pool mal réglé dépasse la VRAM.

### 1.5 Budgets de scène

| Indicateur (pire cas jouable) | PC haut | PC moyen / consoles | Series S / PC min |
|---|---|---|---|
| Draw calls **non Nanite** (passe principale, `stat RHI`) | ≤ 3 000 | ≤ 2 500 | ≤ 1 500 |
| Instances Nanite visibles | quelques centaines de milliers : à mesurer, pas de plafond théorique fixé | idem | idem, avec distances de culling réduites |
| Triangles non Nanite visibles | ≤ 3 M | ≤ 2 M | ≤ 1,2 M |
| Personnages squelettiques animés à pleine fréquence | ≤ 12 | ≤ 10 | ≤ 6 |
| Personnages squelettiques visibles au total (URO/budget) | ≤ 60 | ≤ 45 | ≤ 25 |
| IA « actives » (BT + perception à pleine fréquence) sur le serveur | ≤ 40 | ≤ 40 (dédié) / ≤ 25 (listen) | ≤ 15 (listen sur Series S) |
| IA « réduites » (tick lent) | ≤ 120 | ≤ 100 | ≤ 50 |
| Systèmes Niagara actifs simultanés | ≤ 150 | ≤ 120 | ≤ 70 |
| Lumières dynamiques avec ombres visibles (hors soleil/lune) | ≤ 8 | ≤ 6 | ≤ 3 |
| Pièces de construction visibles (instances) | ≤ 15 000 | ≤ 12 000 | ≤ 8 000 |

---

## 2. Outillage de mesure (rappel commun)

| Besoin | Outil / commande |
|---|---|
| Vue d'ensemble par thread | `stat unit`, `stat unitgraph`, `stat fps` |
| Détail GPU par passe | `stat gpu`, `ProfileGPU` (ou raccourci Ctrl+Maj+Virgule), captures RenderDoc / PIX / Razor selon la plateforme |
| Détail CPU, chargements, tâches | Unreal Insights (`-trace=default` ; ajouter `loadtime,file` pour le streaming) |
| Mémoire | Memory Insights (`-trace=default,memory`), `memreport -full`, `obj list` |
| Rendu / visibilité | `stat SceneRendering`, `stat RHI`, `stat streaming`, modes de visualisation Nanite, Lumen et VSM du viewport |
| Gameplay | `stat game`, `stat anim`, `stat Niagara`, `stat AI` |
| Réseau | `stat net`, Networking Insights (`-trace=default,net` et `-NetTrace=1`) |
| Mesure automatisée | CSV Profiler (`CsvProfile Start` / `CsvProfile Stop`) + PerfReportTool, lancé par la passe automatique de chaque build (§16) |
| World Partition | `wp.Runtime.ToggleDrawRuntimeHash2D` (cellules chargées) |

Règle : on ne profile **jamais** dans l'éditeur pour valider un budget. Les mesures de référence se font en build **Test** (ou Development à défaut), sur la plateforme cible, avec la passe automatique de §16.

---

## 3. Nanite

### 3.1 Usage dans Emberfall

| Contenu | Nanite | Remarque |
|---|---|---|
| Roches, falaises, rochers éboulés | Oui | Cas idéal. Kits modulaires, matériaux opaques. |
| Architecture (ruines, forteresses, caldera de Cendrehaute) | Oui | Préférer des Packed Level Actors (§6.4). |
| Pièces de construction des joueurs | Oui | Rendu par ISM Nanite (§14.1). |
| Troncs d'arbres, grosses branches | Oui | Opaque, sans WPO ou avec WPO coupé à distance. |
| Feuillage d'arbres (cartes masquées + WPO pour le vent) | **À arbitrer** (§3.3) | Coût élevé en 5.4. |
| Herbe, petites plantes | Non (5.4) | Landscape Grass / foliage instancié non Nanite, avec cull distances. |
| Terrain (Landscape) | Option Nanite Landscape (5.x), à tester | Gain surtout sur la géométrie ; vérifier la compatibilité avec la RVT et les Data Layers. |
| Personnages, créatures, dragon | Non | Meshes squelettiques : LOD classiques (§3.4). |
| Eau, verre, effets | Non | Translucide : non pris en charge par Nanite. |

### 3.2 Limites et pièges réels

- **Matériaux masqués (alpha test)** : pris en charge, mais passent par le raster « programmable » de Nanite, nettement plus cher que l'opaque. À réserver aux cas où ils sont indispensables.
- **World Position Offset (WPO)** : pris en charge depuis 5.1, mais (a) coûte au raster, (b) **invalide le cache des Virtual Shadow Maps à chaque frame** pour les pages couvertes (§5.2), (c) peut fausser les bornes de culling si l'amplitude dépasse les bornes déclarées. Mitigations : réglage **« World Position Offset Disable Distance »** sur les composants/instances (le vent s'arrête au-delà, avec un fondu visuel à soigner), « Max World Position Offset Displacement » renseigné, réglage de l'invalidation du cache d'ombre par primitive (§5.2).
- **Petits triangles et feuillage dense** : Nanite gère bien les triangles de la taille d'un pixel, mais **l'overdraw** de multiples couches de feuilles masquées reste coûteux. Vérifier en visualisation Nanite « Overdraw ».
- **Translucidité** : non prise en charge. Tout matériau translucide sur un mesh Nanite le fait retomber sur le mesh de repli (fallback). Séparer les parties translucides (vitraux, eau) dans un mesh non Nanite.
- **Meshes squelettiques** : pas de Nanite en 5.4. Un support expérimental de meshes squelettiques/déformés Nanite apparaît dans des versions plus récentes (à partir de 5.5 selon les notes de version) : **ne pas planifier dessus** tant que la version retenue et la stabilité ne sont pas validées.
- **Mesh de repli (fallback)** : utilisé pour les collisions complexes, Lumen (selon le mode), le ray tracing matériel et les plateformes sans Nanite. Son pourcentage de triangles doit être réglé : un repli trop pauvre dégrade les réflexions HWRT et les collisions complexes.
- **Displacement / tessellation Nanite** : expérimental depuis 5.3 ; non retenu.
- **Spline meshes et Nanite** : la prise en charge dépend de la version (introduite récemment) ; à tester avant usage pour les routes ou palissades.

### 3.3 Végétation : stratégie selon la version

| Version | Arbres proches (< ~50 m) | Arbres moyens (~50-300 m) | Arbres lointains | Herbe |
|---|---|---|---|---|
| **5.4 (base)** | Troncs Nanite opaques ; feuillage en Nanite masqué **ou** non Nanite avec LOD, à trancher par mesure A/B sur la scène de référence n° 2 | Feuillage avec WPO coupé (« WPO Disable Distance »), LOD réduits | HLOD (instancing ou mesh approximatif) ou impostors (§4.3) | Landscape Grass non Nanite, cull distance 30-80 m selon profil |
| **Plus récent** | Epic a introduit une filière « Nanite Foliage » (assemblages et représentation voxel à distance) dans une version récente (5.7 selon les annonces), **en statut expérimental**. Évaluer en prototype isolé uniquement. | idem | idem | idem |

Recommandation : construire les arbres en **assemblages modulaires** (tronc + branches + grappes de feuilles) dès maintenant, car c'est compatible avec les deux filières. Ne pas bâtir le planning sur une fonctionnalité expérimentale.

### 3.4 Ce qui n'est pas Nanite : LOD classiques

| Catégorie | LOD | Règles |
|---|---|---|
| Personnages joueurs | 4 LOD | LOD0 réservé aux cinématiques et au joueur local proche. |
| Créatures communes | 4-5 LOD | Réduction auto (Skeletal Mesh Reduction) + retouche manuelle des silhouettes ; suppression d'os aux LOD bas (« bones to remove »). |
| Dragon Vaelthyrax | 5 LOD + LOD « vol lointain » | Silhouette lisible jusqu'à 1 km ; ailes simplifiées au LOD3+. Animation coûteuse : voir §12.2. |
| Herbe, petites plantes | 2-3 LOD + cull distance | Pas d'ombres dynamiques au-delà de 20-30 m. |
| Meshes translucides | 2-3 LOD | Overdraw contrôlé en visualisation « Shader Complexity ». |

Les distances de LOD se règlent en taille écran (screen size), et sont mises à l'échelle par profil via `r.SkeletalMeshLODBias` et `r.StaticMeshLODDistanceScale` (exposés dans la scalabilité).

---

## 4. HLOD et végétation lointaine

### 4.1 Couches HLOD (World Partition)

| Couche HLOD | Type (option du moteur) | Contenu | Cellule HLOD / portée (cible) |
|---|---|---|---|
| `HLOD_Instanced` | **Instancing** | Rochers, arbres, props répétés : conserve les instances, retire les acteurs | Couvre de la limite de chargement jusqu'à ~1 km |
| `HLOD_Merged` | **Merged Mesh** (ou **Simplified Mesh**) | Architecture et ruines non Nanite | idem |
| `HLOD_Approx` | **Approximated Mesh** | Grands ensembles (forteresses, villages, falaises) en un mesh et une texture uniques | 1 km jusqu'à l'horizon |
| Niveau supérieur | HLOD de HLOD (hiérarchie à plusieurs niveaux, selon la version) | Silhouettes lointaines des biomes | Horizon |

Règles :
- Les HLOD se **regénèrent en pipeline automatique** (commandlet de build HLOD) sur chaque build nocturne, jamais à la main.
- Les acteurs **dynamiques ou détruits** (pièces de construction, piliers de l'arène) sont **exclus** des HLOD (`Include Actor in HLOD` décoché), sinon l'HLOD montre un état faux.
- Les bases des joueurs au-delà de la portée de chargement : représentation **lointaine propre au système de construction** (ISM simplifiées par base, matériau unique), pas d'HLOD (contenu créé en jeu).

### 4.2 Distances cibles

| Élément | PC haut | Consoles 60 | Series S |
|---|---|---|---|
| Fin du chargement complet (grille principale) | 384 m | 256 m | 192 m |
| HLOD instancié | jusqu'à 1,5 km | jusqu'à 1 km | jusqu'à 700 m |
| HLOD approximatif | horizon | horizon | horizon (texture réduite) |

### 4.3 Impostors pour la végétation lointaine

- **Rôle** : remplacer, au-delà de ~300-500 m, les arbres par des billboards octaédriques (8-16 angles précalculés) quand l'HLOD instancié reste trop cher.
- **Production** : outil de baking d'impostors (plugin historique d'Epic / outils tiers) à valider dans la version retenue ; sinon, dernier LOD de l'arbre en « cartes croisées ».
- **Pièges** : éclairage plat (normales et profondeur à baker), transitions visibles (dither de fondu), mémoire des atlas (budget à mesurer).
- **Alternative plus récente** : la représentation voxel lointaine de « Nanite Foliage » (si adoptée, cf. §3.3).

---

## 5. Ombres et éclairage

### 5.1 Virtual Shadow Maps (VSM)

- **Activées** sur PC moyen et plus, PS5, XSX (`r.Shadow.Virtual.Enable 1`). **Cache activé** (`r.Shadow.Virtual.Cache 1`).
- Lumière directionnelle unique (soleil **ou** lune, jamais les deux avec ombres en même temps).
- Réglage de finesse par profil : `r.Shadow.Virtual.ResolutionLodBiasDirectional` (biais positif = moins de pages, moins cher).
- Pool de pages physiques : `r.Shadow.Virtual.MaxPhysicalPages`, à réduire sur Series S et PC min puis vérifier l'absence d'artefacts (pages manquantes) en visualisation VSM.

### 5.2 Invalidations du cache VSM : les sources à maîtriser

| Source d'invalidation | Impact | Mitigation |
|---|---|---|
| **Soleil qui bouge (cycle jour/nuit)** | La rotation de la lumière directionnelle invalide les pages | Mettre à jour la direction du soleil **par paliers** (ex. toutes les 1-2 s de jeu, ou à l'écart angulaire) plutôt qu'à chaque frame ; mesurer le coût de chaque palier. |
| **WPO du feuillage (vent)** | Invalidation permanente des pages couvertes par la végétation | « WPO Disable Distance » ; réglage **Shadow Cache Invalidation Behavior** sur les primitives (ex. traiter comme rigide/statique les éléments dont le WPO est imperceptible dans l'ombre) ; vent réduit sur les ombres lointaines. |
| **Objets dynamiques** (créatures, dragon, joueurs, débris) | Invalidation locale | Normal ; limiter les débris projetant des ombres (débris < 50 cm sans ombre). |
| **Pièces de construction placées/détruites** | Invalidation locale au changement | Acceptable si les composants ISM sont **découpés par tronçon** (§14.1) : modifier une instance ne doit pas invalider toute la base. |
| **Effondrement de l'arène du boss** | Forte invalidation pendant quelques secondes | Accepté : scripté, budget spécifique (scène de référence n° 4). |

### 5.3 Lumen

| Réglage | PC haut | PC moyen | PS5 / XSX Perf 60 | PS5 / XSX Qualité 30 | Series S |
|---|---|---|---|---|---|
| GI dynamique (`r.DynamicGlobalIlluminationMethod`) | Lumen (1) | Lumen (1) | Lumen (1) | Lumen (1) | Lumen (1) bas, **ou** repli (§5.5) |
| Ray tracing matériel (`r.Lumen.HardwareRayTracing`) | 1 (option, défaut selon mesure) | 0 | 0 (1 à évaluer) | 0 (1 à évaluer) | 0 |
| Réflexions (`r.ReflectionMethod`) | Lumen (1) | Lumen (1) | Lumen (1) | Lumen (1) | SSR (2) |
| Final Gather — sous-échantillonnage des probes (`r.Lumen.ScreenProbeGather.DownsampleFactor`) | défaut | défaut | défaut ou +1 cran | défaut | plus grossier |
| Groupe de scalabilité GI (`sg.GlobalIlluminationQuality`) | 3 (Épique) | 2 (Élevé) | 2 | 3 | 1 |
| Groupe réflexions (`sg.ReflectionQuality`) | 3 | 2 | 2 | 3 | 1 |

Règles de contenu pour Lumen :
- **Lumen Scene** : les meshes doivent avoir des Mesh Distance Fields corrects (Lumen logiciel les utilise) ; murs trop fins (< 10 cm) = fuites de lumière. Les pièces de construction doivent avoir une épaisseur suffisante — **contrainte à transmettre à Dev B et aux artistes**.
- Intérieurs sombres (bases, cryptes) : Lumen a besoin de temps pour converger ; prévoir des lumières d'appoint jouables (torches, foyers) plutôt que de compter sur le rebond seul.
- **Émissifs** (souffle du dragon, braises) : ne pas compter sur Lumen pour éclairer la scène par un émissif rapide et petit (bruit, latence) ; ajouter des lumières dynamiques courtes (pool, §11.1).

### 5.4 Groupes de scalabilité (`sg.*`) par profil

Valeurs 0 = Bas, 1 = Moyen, 2 = Élevé, 3 = Épique, 4 = Cinématique.

| Groupe | PC haut | PC moyen | PC min | PS5/XSX Perf | PS5/XSX Qualité | Series S |
|---|---|---|---|---|---|---|
| `sg.ViewDistanceQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.AntiAliasingQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.ShadowQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.GlobalIlluminationQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.ReflectionQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.PostProcessQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.TextureQuality` | 3 | 2 | 1 | 3 | 3 | 1 |
| `sg.EffectsQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.FoliageQuality` | 3 | 2 | 1 | 2 | 3 | 1 |
| `sg.ShadingQuality` | 3 | 2 | 1 | 2 | 3 | 1 |

Les profils consoles se définissent dans les fichiers de configuration de plateforme (`*Scalability.ini` / Device Profiles), les valeurs par niveau (ce que « 2 » signifie pour Lumen, VSM, etc.) dans `DefaultScalability.ini` du projet. **Tout cvar de rendu modifié par le projet passe par ces fichiers**, jamais par du code dispersé.

### 5.5 Series S : alternatives à évaluer au jalon Vertical Slice

| Option | GI | Réflexions | Ombres | Coût GPU visé | Risque |
|---|---|---|---|---|---|
| **A (préférée)** | Lumen logiciel, `sg.GlobalIlluminationQuality 1`, probes plus grossières | SSR | VSM, cache actif, biais de résolution fort | GI ≈ 5-6 ms à 30 fps | Bruit et latence d'éclairage la nuit et en intérieur |
| **B (repli)** | Pas de Lumen : GI écran (`r.DynamicGlobalIlluminationMethod 2`) + skylight dynamique + Distance Field AO (`r.DistanceFieldAO 1`) | SSR | VSM ou ombres en cascade (`r.Shadow.Virtual.Enable 0`) + ombres à distance par champs de distance | GI ≈ 2-3 ms | Rendu visiblement différent ; intérieurs des bases à éclairer à la main ; double validation artistique |

Décision au jalon Vertical Slice sur mesure réelle (scènes de référence n° 2 et n° 4). L'option B impose que la direction artistique valide **deux** rendus : c'est un coût de production à prévoir dès maintenant.

### 5.6 Météo, ciel, brouillard

- Sky Atmosphere + Volumetric Clouds + Exponential Height Fog avec brouillard volumétrique : coûts à suivre (ligne « ciel » du §1.3). Sur Series S, nuages volumétriques à qualité réduite ou couche de nuages 2D en repli.
- La pluie : Niagara GPU **attaché à la caméra** (volume local), pas un système à l'échelle du monde. Mouillé des surfaces par paramètre global de matériau (Material Parameter Collection), pas par décalques.
- Éclairs (tempête de cendres de la phase 3) : lumière directionnelle secondaire **sans ombre** ou flash de post-process ; jamais une lumière ombrée à grand rayon.

---

## 6. World Partition

### 6.1 Hypothèse de taille du monde

Monde de **l'ordre de 8 × 8 km** (hypothèse : la taille réelle n'est pas encore fixée dans le GDD). Les valeurs ci-dessous sont à recalibrer si la taille change.

### 6.2 Grilles de streaming (runtime grids)

| Grille | Taille de cellule | Rayon de chargement (cible, PC haut / consoles / Series S) | Contenu |
|---|---|---|---|
| `MainGrid` (défaut) | 128 m (12 800 cm, valeur par défaut du moteur) | 384 / 256 / 192 m | Architecture, gros props, spawners de créatures, points d'intérêt |
| `DetailGrid` | 64 m | 128 / 96 / 64 m | Petits props, décor de sol, interactifs mineurs, récoltables |
| `LandmarkGrid` | 256 m | 1 024 / 768 / 512 m | Grands repères visibles de loin (tours, caldera), à charger tôt |
| Toujours chargé | — | — | Gestionnaires, acteurs de jeu globaux (`AWorldDataLayers`, météo, cycle jour/nuit). **À garder minimal.** |

Règles :
- Le **terrain** est géré par les proxies de streaming de Landscape (taille de composant/région à aligner avec la grille).
- **One File Per Actor (OFPA)** activé : chaque acteur placé est un fichier, ce qui permet à plusieurs personnes de travailler sur la même zone sans verrouiller la carte. Conséquence : des dizaines de milliers de fichiers ; le contrôle de version doit être Perforce (ou Git LFS avec verrouillage) — **décision outillage à confirmer**.
- Mesure : temps de chargement des cellules en Unreal Insights (`loadtime`) ; **aucune cellule ne doit provoquer de pic > 4 ms sur le game thread** (cible). Les acteurs à `BeginPlay` coûteux sont refactorés en initialisation différée.
- **Pas de rechargement à la course** : à la vitesse de déplacement max (monture/sprint), le chargement doit garder au moins une cellule d'avance. Le vol du dragon (2 400-4 500 cm/s) concerne une arène fermée, pas le monde ouvert.

### 6.3 Data Layers

| Data Layer (runtime) | Type | Usage |
|---|---|---|
| `DL_Biome_<Nom>` | Runtime, activé selon la zone | Contenu propre à un biome (ambiances, faune, Niagara d'ambiance) chargé seulement dans le biome |
| `DL_Event_<Nom>` | Runtime, activé par le serveur | Événements de monde (camp de bandits, invasion) |
| `DL_Arena_P1` / `DL_Arena_P2` / `DL_Arena_P3` | Runtime, piloté par `AEMBDragonArena` côté serveur | États de la caldera : intacte (6 piliers, 3 ponts) ; coulées de lave, 3 piliers détruits ; île centrale de rayon 4 000 cm + tempête |
| `DL_Season_<Saison>` / `DL_Weather_<État>` | Runtime | Variantes de décor saisonnier (neige au sol, feuillage) ; la météo pure reste paramétrique (MPC), pas en Data Layer |
| `DL_Editor_*` | Éditeur seulement | Aides de travail, jamais chargées en jeu |

- L'état des Data Layers runtime est **décidé par le serveur** et répliqué aux clients (via le gestionnaire de Data Layers du monde). Vérifier le comportement exact dans la version retenue et le **tester en coop dès le Vertical Slice** (client qui rejoint en cours de phase 3 : il doit charger `DL_Arena_P3`, pas P1).
- Passage P1 → P2 → P3 : l'activation d'un Data Layer charge du contenu ; la transition de phase (invulnérabilité 5 s, rugissement) **masque** ce chargement. Le contenu P2/P3 est passé à l'état **Loaded** (chargé, non visible) dès l'entrée dans l'arène, puis **Activated** à la transition — aucun chargement disque pendant le combat.

### 6.4 Level Instances et Packed Level Actors

- **Packed Level Actor** : pour tout ensemble statique répété (ruine type, maison de village, segment de rempart). Il convertit l'ensemble en ISM/HISM : peu de draw calls, peu d'acteurs.
- **Level Instance** : pour les ensembles qui contiennent de la logique ou des acteurs dynamiques (camp avec spawners, donjon).
- Règle : un ensemble utilisé 3 fois ou plus devient un Packed Level Actor.

### 6.5 Streaming en multijoueur

- Chaque `PlayerController` est une **source de streaming**. Sur le serveur, les cellules chargées sont l'**union** des sources des 4 joueurs.
- **Serveur dédié** : vérifier dans la version retenue le comportement par défaut (selon la configuration, le serveur peut charger tout le monde ou streamer selon les joueurs). Les cvars `wp.Runtime.EnableServerStreaming` et `wp.Runtime.EnableServerStreamingOut` pilotent ce comportement ; **à valider en version cible**. Cible : streaming serveur activé, déchargement activé, pour tenir dans la mémoire d'une instance de serveur.
- **Listen server** : l'hôte charge l'union des zones des 4 joueurs **en plus** de son rendu. Pire cas : 4 joueurs aux 4 coins de la carte = 4 zones de simulation sur une console. Sur Series S c'est **un risque mémoire et game thread majeur** (§8.6).
- Les clients ne chargent que leur propre zone : un acteur répliqué dont la cellule n'est pas chargée chez le client est créé à la volée (s'il est dynamique), ce qui exige que les acteurs répliqués **ne dépendent pas** d'acteurs placés non chargés.

---

## 7. Culling et occlusion

| Mécanisme | Emberfall | Commentaire |
|---|---|---|
| **Culling Nanite** (frustum + occlusion HZB en deux passes) | Automatique | Très efficace ; ne concerne que les meshes Nanite. |
| **HZB pour le non Nanite** (`r.HZBOcclusion`) | À comparer aux requêtes matérielles | Moins précis mais sans latence de lecture GPU ; décision par mesure. |
| **Requêtes d'occlusion matérielles** (`r.AllowOcclusionQueries 1`) | Actif par défaut | Pour personnages, herbe, props non Nanite. Coût réel quand il y a des milliers de petites primitives : regrouper en ISM. |
| **Occlusion logicielle** | Non retenue | Historiquement destinée au mobile. |
| **Precomputed visibility** | **Non pertinente** | Nécessite un précalcul statique de cellules de visibilité depuis des zones jouables connues ; le monde ouvert, le cycle jour/nuit, les bases construites et l'arène qui s'effondre la rendent fausse ou inutile, et elle ne s'intègre pas au flux World Partition. |
| **Cull distances par composant** (`Max Draw Distance`) et **Cull Distance Volumes** | Oui, systématique sur le non Nanite | Table de base (cible) : props < 0,5 m → 30-50 m ; < 2 m → 80-120 m ; < 5 m → 200 m ; au-delà → HLOD. Mise à l'échelle par profil via `r.ViewDistanceScale` (piloté par `sg.ViewDistanceQuality`). |
| **Densité de végétation** | `foliage.DensityScale`, `grass.DensityScale` (via `sg.FoliageQuality`) | Réduction sur PC min et Series S. |
| **Distance de pertinence réseau** (`NetCullDistanceSquared`) | Par classe d'acteur | Voir §13.2. À ne pas confondre avec le culling visuel. |

Vérification : `r.VisualizeOccludedPrimitives 1` pour voir ce qui est occulté, `stat SceneRendering` (primitives visibles, requêtes d'occlusion).

---

## 8. Textures 4K, Virtual Textures, mémoire de streaming

### 8.1 Politique de résolution

| Catégorie | Résolution source | Résolution max en jeu (PC haut / consoles / Series S) |
|---|---|---|
| Héros (joueurs, dragon, boss) | 4K | 4K / 4K / 2K |
| Architecture et falaises « hero » | 4K | 4K / 2K-4K / 2K |
| Matériaux de terrain tuilables | 2K-4K | 2K-4K / 2K / 1K-2K |
| Props communs, pièces de construction | 2K | 2K / 2K / 1K |
| Petits props, débris | 1K | 1K / 1K / 512 |

La « 4K partout » n'est pas un objectif : le **mip effectivement streamé** dépend de la taille à l'écran. On source en 4K pour les héros ; le groupe de textures (LOD Group) plafonne par plateforme (Device Profiles), ce qui évite des textures de 4K inutiles sur Series S.

### 8.2 Compression

| Usage | Format (PC / consoles) |
|---|---|
| Albédo (avec ou sans alpha) | BC7 (qualité) ; BC1 pour les textures secondaires sans alpha |
| Normales | BC5 |
| Masques mono-canal (rugosité, AO, hauteur séparés) | BC4 |
| Masques empilés (ORM : AO/Roughness/Metal) | BC7 ou BC1 selon la qualité attendue |
| HDR (ciel, cubemaps) | BC6H |
| **ASTC** | Formats mobiles : **hors cible** (aucune plateforme mobile prévue). À reconsidérer seulement si un portage mobile ou Switch 2 est décidé. |

- **Oodle Texture** (encodage RDO) activé en build de livraison : réduit fortement la taille sur disque et le temps de chargement **après** compression Oodle Data du paquet ; le niveau de RDO (lambda) se règle par groupe de textures, avec validation visuelle de la DA.
- **Oodle Data** (Kraken ou Mermaid) pour la compression des paquets (.pak/IoStore).

### 8.3 Streaming Virtual Textures (SVT) et Runtime Virtual Textures (RVT)

- **RVT pour le terrain** : le matériau de terrain multi-couches est rendu dans une RVT (couleur, normale, rugosité), ce qui rend le coût par pixel indépendant du nombre de couches. Une RVT de hauteur sert aussi au **fondu des pieds de rochers et de pièces de construction** avec le sol.
- **SVT** pour les très grandes textures uniques (atlas d'HLOD, cartes) : à utiliser avec discernement ; un abus de SVT déplace le coût vers les pools de VT et le feedback GPU.
- Pools de VT : à dimensionner par plateforme (Device Profiles) et à surveiller (débordement = flou).

### 8.4 Pool de streaming de textures

- `r.Streaming.PoolSize` par profil (§1.4) ; `stat streaming` doit montrer **0 Mo « over budget »** dans les scènes de référence.
- Textures d'UI et textures toujours visibles : `NeverStream` limité au strict nécessaire (elles sortent du pool et consomment en permanence).
- Mesure du mip demandé vs mip chargé avec `stat streaming` et `ListTextures` ; chercher les textures 4K qui ne descendent jamais.

### 8.5 Mémoire des meshes

- Nanite stocke les données en streaming (pool de streaming Nanite) : surveiller la mémoire Nanite en Memory Insights.
- Collisions : collision simple pour tout ce qui est petit ; la collision complexe (« Use Complex as Simple ») est **interdite** sur les meshes Nanite détaillés (le repli serait trop lourd ou trop grossier).

### 8.6 Risque Series S en listen server (mémoire)

Un hôte Series S qui charge 4 zones distantes peut dépasser son budget. Options, à trancher en ADR (chef d'équipe + réseau) :
1. **Serveurs dédiés** pour les sessions dont l'hôte est sur Series S (coût d'hébergement récurrent : à chiffrer).
2. **Laisse de groupe** : les joueurs ne peuvent pas s'éloigner de plus de N km de l'hôte (contrainte de game design).
3. **Plafond de joueurs** à 2 ou 3 quand l'hôte est une Series S.
4. **Streaming serveur dégradé** : chez l'hôte, les zones distantes sont simulées en mode réduit (IA gelée, pas de rendu) — réduit le coût mais pas la mémoire des cellules.

---

## 9. Asset Manager, références souples, chargement asynchrone

### 9.1 Règles

- **Primary Assets** : créatures, armes, armures, pièces de construction (`DT_BuildPieces` ou Data Assets par pièce), boss, biomes, objets. Chacun a un **Primary Asset ID** (`Type:Nom`) déclaré dans les réglages de l'Asset Manager (types à scanner).
- **Références dures interdites** depuis les classes « hub » (personnage joueur, GameMode, GameState, HUD, inventaire, composant de construction) vers du contenu lourd (meshes de créatures, Niagara du dragon, montages du boss). Ces classes utilisent `TSoftObjectPtr` / `TSoftClassPtr`.
- **Chargement asynchrone** via `FStreamableManager` (ou l'API de l'Asset Manager pour les Primary Assets), avec **bundles** (`AssetBundles` en métadonnées : par ex. `UI` pour icônes et textes, `Game` pour meshes et sons, `Boss` pour l'arène) : l'inventaire charge le bundle `UI` d'un objet sans charger son mesh.
- **Préchargement par contexte** : à l'approche de la caldera, chargement asynchrone du bundle `Boss` (dragon, Niagara des souffles, montages, sons) déclenché par un volume d'approche, pour qu'il soit en mémoire avant la cinématique d'entrée.
- **Blueprints** : un Blueprint qui « cast » vers une classe lourde crée une référence dure. Préférer interfaces et classes de base C++ légères.

### 9.2 Contrôle

| Contrôle | Outil | Seuil (cible) |
|---|---|---|
| Taille mémoire chargée par une classe hub | Size Map (éditeur) | `BP_EMBPlayerCharacter` ≤ 150 Mo de dépendances |
| Chaînes de références | Reference Viewer | Aucune référence du personnage joueur vers le dragon |
| Audit global | Asset Audit / chunking | Revue à chaque jalon |
| Chargements synchrones en jeu | Unreal Insights (`loadtime`) | **Zéro** chargement synchrone en exploration et en combat |

---

## 10. Shaders et PSO

- **Problème** : la première utilisation d'une combinaison shader + état de pipeline (PSO) provoque une compilation à la volée sur PC (DX12/Vulkan) = saccade.
- **Stratégie double** :
  1. **PSO precaching** (`r.PSOPrecaching`, disponible en 5.2+ ; activation par défaut selon la version, **à vérifier**) : les PSO probables sont compilés lors du chargement des composants, avant affichage. Mesurer le taux de « PSO manqués » en Insights.
  2. **Bundled PSO cache** (`r.ShaderPipelineCache.Enabled 1`) : on enregistre les PSO réellement utilisés en jouant (build lancé avec `-logPSO`), on agrège les fichiers collectés et on les embarque dans le build ; ils sont compilés au premier lancement (écran de préparation des shaders) ou en tâche de fond.
- **Collecte automatisée** : chaque build de jalon passe par un parcours scripté (voyage de caméra + combat de chaque créature + phases du boss + construction/effondrement + météo) qui enregistre les PSO.
- **Écran de préchauffage** au premier lancement sur PC, avec progression et possibilité de jouer en compilant en tâche de fond (option).
- **Réduction à la source** : nombre de permutations limité (pas de « static switch » en cascade dans les matériaux maîtres), **matériaux maîtres** uniques par famille (roche, bois, métal, végétation, créature, peau du dragon) et instances de matériaux partout ailleurs. Revue des statistiques de shaders par l'équipe tech art.
- **Consoles** : les PSO sont compilés hors ligne ; le problème de saccade de compilation ne s'y pose pas de la même façon, mais le **nombre de permutations** reste un coût de build et de mémoire.
- **Critère** : sur PC, aucune saccade > 50 ms attribuable aux PSO pendant la passe automatique, à partir du jalon Beta.

---

## 11. Pools d'acteurs et d'effets

### 11.1 Acteurs à pooler

| Élément | Taille de pool cible (PC / consoles / Series S) | Côté | Remarque |
|---|---|---|---|
| Flèches et carreaux | 64 / 64 / 48 | Serveur + client | Flèche plantée = instance ISM après 2 s, retour au pool de l'acteur |
| Projectiles magiques et créatures | 48 / 48 / 32 | Serveur (+ prédiction cosmétique client) | |
| **Zones de feu du dragon** (`AerialStrafe` : zones 8 s ; `EmberRain` : 12 impacts) | 32 / 32 / 24 | Serveur (dégâts) ; visuel côté client | Cible : un acteur de zone = une forme de collision + un Niagara du pool ; pas de lumière ombrée. **Code v1.1** : pas d'acteur — les zones sont des données serveur (`FEMBDragonFireZone`, timer 4 Hz) et leur visuel est créé chez les clients par `Multicast_SpawnFireZoneVFX` via `SpawnSystemAtLocation`, **sans pool** (écart à D8, voir `02_Architecture.md` I11) |
| **Débris d'effondrement** | 128 / 128 / 64 | **Client seulement** (cosmétique) | Voir §11.3 |
| Lumières dynamiques courtes (impacts, souffle) | 16 / 16 / 8 | Client | Sans ombre au-delà de 2 simultanées |
| Décalques d'impact (brûlures, sang) | 64 / 64 / 32 | Client | Recyclage du plus ancien |
| Numéros de dégâts / UI flottante | 32 | Client | Widgets réutilisés |

Règles :
- Le pool est **préchauffé** au chargement de la zone (ou du bundle `Boss`), jamais au premier tir.
- Un objet rendu au pool est désactivé (tick, collision, visibilité, réplication) et réinitialisé ; aucun `Destroy` en combat.
- Pool plein : on **recycle le plus ancien** (débris, décalques) ou on **refuse** l'effet le moins important (lumières), jamais d'allocation supplémentaire.

### 11.2 Niagara

- Appel de spawn avec **méthode de pooling** (AutoRelease / ManualRelease) pour tous les effets répétés.
- **Niagara Effect Types** par famille (combat, ambiance, boss, météo) avec : distance de culling, nombre max d'instances, gestion de la significance (les plus proches/importants d'abord), règles de scalabilité par niveau de `sg.EffectsQuality`.
- Simulation **GPU** pour les gros volumes (braises, pluie, cendres), **CPU** pour ce qui lit des données de gameplay ou est peu nombreux.
- **Souffles du dragon** (`BreathCone` 2,5 s, `BreathSweep`) : un système unique par souffle, collision des dégâts gérée **en gameplay** (cône/forme sur le serveur, ticks à 8/s), jamais par les particules. Translucidité : particules larges et peu nombreuses plutôt que nombreuses et petites (overdraw) ; rendu en résolution réduite si l'option de translucidité séparée basse résolution est validée.
- Budget `stat Niagara` et `stat gpu` (ligne translucidité) mesurés dans la scène de référence n° 4.

### 11.3 Effondrement : débris

- Le **serveur** décide quelles pièces s'effondrent (au plus `MaxCollapsePerFrame` = 32 par vague, vagues espacées de `CollapseWaveDelay` = 0,08 s) et les retire de l'état répliqué de la base (code v1.1 : un acteur par pièce, `bCollapsed` répliqué + `Multicast_PlayCollapseEffects` Unreliable).
- Chaque **client** joue l'effondrement localement : pour chaque pièce retirée, il prend 1 à 3 débris du pool (mesh simplifié du matériau, corps rigide Chaos simple, sans fracture dynamique), les lance, puis les rend au pool après 6-10 s (fondu) ou dès qu'ils dorment.
- Les débris **ne sont pas répliqués**, n'infligent pas de dégâts et ne bloquent pas la navigation (canal de collision dédié : collision avec le monde, pas avec les pawns, ou poussée légère).
- Les **Geometry Collections** (fracture Chaos précalculée) sont réservées aux destructions **scriptées** de l'arène (piliers de basalte, ponts, pourtour de la caldera), préchargées et jouées une fois. **Écart du code v1.1** : `AEMBBuildablePiece::CollapseDebrisClass` (acteur de débris optionnel, Geometry Collection possible) peut encore être utilisé pour les **8 premières pièces de chaque lot** (`MaxHeavyDebrisPerWave`, jamais sur serveur dédié), sans pool. À aligner sur cette règle (laisser la propriété vide, ou la remplacer par le pool de débris).
- Si le pool est vide : la pièce disparaît dans un effet de poussière (Niagara) sans débris physique. Le jeu reste correct, seule la richesse visuelle baisse.

---

## 12. Significance, animation et IA

### 12.1 Significance Manager

Un `USignificanceManager` (plugin Significance Manager) calcule, pour chaque créature, un score = f(distance au joueur local le plus proche, visibilité à l'écran, rôle — boss > élite > commun, est en combat). Côté client : pilote animation et effets. Côté serveur : pilote l'IA (avec la distance au **joueur le plus proche** parmi les 4).

| Palier | Condition (cible) | Animation | IA (serveur) |
|---|---|---|---|
| **S0 — Critique** | Boss ; ou en combat avec un joueur ; ou < 15 m | Pleine fréquence, LOD normal | BT + perception pleine fréquence |
| **S1 — Proche** | 15-50 m, visible | URO actif (1 frame sur 2 à 1 sur 3 selon distance) | Tick IA 0,1-0,2 s |
| **S2 — Loin** | 50-150 m ou non visible | URO fort, `OnlyTickMontagesWhenNotRendered` si invisible | Tick IA 0,5 s, perception réduite |
| **S3 — Dormant** | > 150 m de tout joueur | Pas de tick | **IA dormante** : BT en pause, mouvement simulé grossièrement ou figé, réveil sur entrée en S2 |

### 12.2 Réglages animation

| Réglage | Usage |
|---|---|
| **URO** (Update Rate Optimizations, `bEnableUpdateRateOptimizations` sur le Skeletal Mesh Component) | Activé sur toutes les créatures communes et PNJ ; désactivé sur les joueurs et le dragon en combat |
| **`VisibilityBasedAnimTickOption`** | Créatures : `OnlyTickMontagesWhenNotRendered` (les montages de gameplay — attaques, notifies de fenêtres de coup — continuent quand la créature est hors écran) ; décor animé : `OnlyTickPoseWhenRendered`. **Attention serveur dédié** : il ne rend rien ; vérifier que les notifies de fenêtres de coup (`EMBAnimNotifyState_HitWindow`) s'exécutent bien sur le serveur — joueurs et boss en `AlwaysTickPoseAndRefreshBones` sur le serveur. |
| **Animation Budget Allocator** (plugin, `USkeletalMeshComponentBudgeted`) | Pour les créatures communes : budget global d'animation en ms (`a.Budget.Enabled`, `a.Budget.BudgetMs`), 1,5-2,0 ms (cible) à 60 fps, réduit sur Series S |
| **Tick intervals** | Composants non critiques (perception, sons d'ambiance, boucles de comportement) avec un intervalle de tick > 0 ; tick désactivé par défaut sur les acteurs qui n'en ont pas besoin |
| **Animation multithread** | Anim Blueprints compatibles « Fast Path » et mise à jour multithread (pas de logique Blueprint lourde dans l'Event Graph) |
| **Dragon** | Squelette lourd (ailes, queue, cou) : LOD d'os, physique d'ailes (Rigid Body) seulement aux LOD 0-1, IK pieds désactivée en vol. Budget cible : 1,0 ms de game thread + worker pour l'animation du dragon à 60 fps. |

### 12.3 IA

- **Behavior Trees** : tick des services à intervalle (0,2-0,5 s), pas à chaque frame. `BTService_EMBUpdateCombatContext` du dragon : 0,1-0,2 s.
- **Perception** : nombre de cibles limité à 4 joueurs + quelques PNJ ; intervalles de vue réduits en S1/S2.
- **Navigation** : NavMesh **dynamique** à cause des bases construites (mode « Dynamic Modifiers Only » ou reconstruction par tuiles autour des bases) ; invoqueurs de navigation (Navigation Invokers) pour ne générer le navmesh qu'autour des joueurs en monde ouvert. Coût de reconstruction suivi en Insights (cible : < 1 ms/frame en moyenne, étalé sur les workers).
- **Foules** (meutes de loups, nuées) : si plus de ~50 entités d'un même type, évaluer Mass Entity (ECS d'Unreal) pour les entités lointaines — **non retenu par défaut** (complexité), à rouvrir si le budget IA n'est pas tenu.

---

## 13. Réseau

### 13.1 Architecture de réplication

| Version / option | Statut et position |
|---|---|
| **Pilote de réplication standard** (relevancy + priorité) | **Retenu par défaut** pour 4 joueurs |
| **Push Model** (`net.IsPushModelEnabled`) | Retenu : les propriétés répliquées sont marquées sales explicitement, ce qui évite de comparer toutes les propriétés à chaque mise à jour. **Pas encore appliqué dans le code v1.1** (aucun marquage explicite, aucun réglage `.ini`) : voir `02_Architecture.md` § 9.2 (I15) |
| **Replication Graph** | Plugin éprouvé (Fortnite). À adopter **seulement** si `stat net` / Networking Insights montrent un coût de réplication serveur > 2 ms par frame (cible) dans les scènes de référence |
| **Iris** (nouveau système de réplication, `net.Iris.UseIrisReplication`) | Expérimental dans les premières versions d'UE5 ; **statut à vérifier dans la version retenue** avant toute adoption. Non retenu pour 5.4. |

### 13.2 Fréquences de mise à jour (cibles)

| Classe d'acteur | `NetUpdateFrequency` / min | `NetCullDistanceSquared` (distance) | Dormance |
|---|---|---|---|
| Joueur (`AEMBPlayerCharacter`) | 60 / 30 Hz | Toujours pertinent pour la session (4 joueurs) | Jamais |
| Dragon | 30 / 15 Hz | Toujours pertinent dans l'arène | Jamais pendant le combat |
| Créatures communes | 10 / 2 Hz (adaptatif) | 150 m | Dormant en S3 |
| Projectiles | 30 Hz pendant le vol, puis désactivé | 150 m | — |
| Zones de feu | 2 Hz (état fixe) | 200 m | Dormant après création (cible si elles deviennent des acteurs ; **code v1.1** : non répliquées, données serveur + un multicast de VFX par traînée) |
| **Base de construction** (un acteur par base, §13.4) | 1-2 Hz | 300 m | **Dormant** ; réveil par `FlushNetDormancy` sur modification |
| Coffres, établis, portes | 2 Hz | 50 m | Dormant hors interaction |
| Récoltables (arbres, rochers) | état seulement | 100 m | Dormant ; état récolté en Fast Array par zone |

- Fréquence adaptative (`net.UseAdaptiveNetUpdateFrequency`) : à activer et mesurer.
- **Serveur dédié** : `NetServerMaxTickRate` à 30 Hz (cible), à monter à 60 Hz si la sensation de combat l'exige (parades à 10 frames de fenêtre à 60 fps : la validation serveur des fenêtres doit tolérer la latence — **sujet à traiter avec Dev A**).
- **Listen server** : le serveur tourne à la fréquence de l'hôte (30 ou 60 fps selon la plateforme) : un hôte Series S impose 30 Hz de simulation serveur à tout le groupe.

### 13.3 Bande passante (cibles)

| Flux | Moyenne | Pic (combat boss + effondrement) |
|---|---|---|
| Serveur → chaque client | ≤ 30 Ko/s | ≤ 80 Ko/s |
| Client → serveur | ≤ 8 Ko/s | ≤ 15 Ko/s |
| Montant total d'un hôte listen (3 clients) | ≤ 90 Ko/s (≈ 0,75 Mbit/s) | ≤ 240 Ko/s (≈ 2 Mbit/s) |

Les débits maximum du pilote réseau (`MaxClientRate`, `MaxInternetClientRate`, vitesse configurée du joueur) doivent être relevés au-dessus de ces pics dans `DefaultEngine.ini` ; **vérifier les valeurs par défaut de la version retenue**. Mesure : `stat net`, Networking Insights.

### 13.4 Réplication des pièces de construction

Problème : des milliers de pièces × 4 joueurs. Un acteur répliqué par pièce, même dormant, coûte en mémoire serveur, en temps de réveil et en canaux réseau à l'arrivée d'un client.

Recommandation (ADR à valider avec Dev B, qui possède `EMBBuildablePiece`, `EMBStructureSubsystem`) :
- **Un acteur répliqué `Base` par base** (ou par cluster de 32 × 32 m pour les très grandes bases), contenant un **Fast Array** (`FFastArraySerializer`) de pièces : identifiant de pièce, type (ligne de `DT_BuildPieces`), transform quantifiée sur la grille (`GridSize` 300 cm, rotation par pas de 90°/15°), PV, intégrité éventuellement recalculée côté client.
- Seules les **modifications** (ajout, retrait, PV) sont envoyées (delta) ; l'acteur est dormant et réveillé par `FlushNetDormancy` à chaque modification.
- Côté client et serveur, le rendu et la collision passent par des **ISM** (§14.1).
- `AEMBBuildablePiece` (acteur) reste utilisé **uniquement** pour l'aperçu de placement et les pièces interactives (portes, établis, coffres).
- Client qui rejoint : reçoit le Fast Array complet de chaque base pertinente (≤ 300 m) une fois ; cible ≤ 50 Ko pour une base de 3 000 pièces (≈ 16 octets par pièce après quantification, à vérifier).

---

## 14. Construction : rendu, limites, intégrité, effondrement

### 14.1 Rendu instancié

- Une base = un ensemble de **composants ISM**, un par (mesh × matériau) **et par tronçon spatial** (ex. 16 × 16 m), Nanite activé.
- Pourquoi des tronçons : ajouter/retirer une instance modifie le composant ; un tronçon limite le coût de mise à jour du proxy de rendu, des bounds, de l'invalidation VSM et de la reconstruction du navmesh.
- **ISM plutôt que HISM** pour les meshes Nanite (Nanite fait son propre culling par instance ; la hiérarchie du HISM n'apporte rien et sa reconstruction coûte). HISM conservé pour le non-Nanite (repli PC min, si Nanite est désactivé). **À confirmer par mesure dans la version retenue.**
- Collision : collision simple par instance (boîtes) ; les traces de placement (`GroundTraceDepth`, `SocketSnapRadius`) utilisent un canal dédié.
- **Fusion** (merge) : pour les bases « finies » inactives depuis longtemps, une fusion en mesh unique n'est **pas** retenue (contenu modifiable à tout moment, coût de génération à l'exécution) ; l'ISM Nanite suffit. La représentation lointaine (§4.1) est la seule simplification.

### 14.2 Limites (cibles à valider en test de charge)

| Limite | Valeur cible | Justification |
|---|---|---|
| Pièces par base | 3 000 | Intégrité < 1 ms en pire cas étalé ; Fast Array raisonnable |
| Bases par joueur | 3 | Contrôle de la charge serveur |
| Pièces totales par monde (serveur) | 15 000 (dédié) / 10 000 (listen) / 6 000 (hôte Series S) | Mémoire et sauvegarde |
| Pièces de mobilier par base | 300 | Draw calls non Nanite (mobilier détaillé souvent masqué/translucide) |
| Lumières dynamiques par base | 8 (seules 2-3 avec ombres, les plus proches) | GPU |

Les limites sont **exposées dans la configuration serveur** (data, pas code en dur) et affichées au joueur (compteur de pièces dans l'interface de construction). **État du code v1.1** : 3 000 par base et 15 000 par monde sont appliquées par le serveur (résultat `LimitReached`), avec une seule valeur réglable en Blueprint (`UEMBBuildingComponent::MaxPiecesPerBase / MaxPiecesPerWorld`), sans distinction listen / Series S ; les limites « bases par joueur », mobilier et lumières ne sont pas codées, et aucun compteur n'est affiché (pas d'UI).

### 14.3 Calcul d'intégrité (serveur)

- Rappel du modèle (constantes §5) : intégrité d'une pièce = max sur ses supports (intégrité du support − décroissance du matériau selon la direction) ; fondations au sol = `GroundedIntegrity` 100.
- **Incrémental** : à l'ajout d'une pièce, on calcule son intégrité depuis ses voisins puis on ne relaxe que les pièces dont l'intégrité augmente. Au retrait ou à la destruction, on recalcule **uniquement le sous-graphe dépendant** (code v1.1 : chaque pièce mémorise son support parent ; le sous-arbre des pièces dont le chemin passait par la pièce retirée est remis à zéro, réamorcé depuis ses voisins non affectés et résolu par un Dijkstra local, O(s log s) avec s = taille du sous-arbre), pas toute la base. Les destructions d'une même frame sont regroupées en un seul retrait et un seul recalcul.
- **Étalement** : cible de **0,5 ms/frame** sur le serveur pour la propagation, avec reprise à la frame suivante au-delà. **Non codé en v1.1** : le recalcul du sous-graphe dépendant s'exécute en une seule passe dans la frame de la destruction ; seules les destructions en cascade sont étalées (vagues de 32 pièces toutes les 0,08 s, sans nouveau recalcul). À mesurer sur la scène de référence n° 3 avant de décider s'il faut découper le recalcul.
- Structures de données : graphe d'adjacence par base (indices entiers, pas de pointeurs d'acteurs), recalculé au chargement de la sauvegarde, jamais répliqué (le client peut le recalculer pour l'aperçu de couleur).
- Mesure : compteur Insights dédié « Integrity » ; test de charge = base de 3 000 pièces en pierre dont on détruit la fondation centrale.

### 14.4 Effondrement

- File des pièces à effondrer, triée **par niveau** (profondeur depuis la cassure, puis distance), traitée à **32 pièces par vague maximum** (`MaxCollapsePerFrame`), une vague toutes les `CollapseWaveDelay` = 0,08 s (timer, pas de Tick) ; une pièce « sauvée » entre-temps par un nouveau support est ignorée.
- Coût serveur par pièce effondrée : retrait du Fast Array, retrait d'instance ISM, mise à jour du navmesh local (regroupée par tronçon et par vague, pas par pièce).
- Coût client : retrait d'instance + débris du pool (§11.3).
- Cible : un effondrement de 500 pièces s'étale sur ≥ 16 vagues (32 par vague, soit environ 1,3 s à 0,08 s d'intervalle) sans dépasser **+2 ms** de game thread et **+2 ms** de GPU sur les frames concernées, sur toutes les plateformes. Attention : la frame de la cassure porte en plus le recalcul complet du sous-graphe dépendant (§ 14.3).

---

## 15. Scalabilité exposée au joueur

| Option (menu) | PC | Consoles |
|---|---|---|
| Préréglage global (Bas / Moyen / Élevé / Épique / Personnalisé) | Oui (`sg.*`) | — |
| Mode Performance / Qualité | — | PS5, XSX (Series S : mode unique) |
| Qualité par groupe (distance de vue, ombres, GI, réflexions, textures, effets, végétation, post-process, ombrage) | Oui | — |
| Méthode d'upscaling (TSR, DLSS, FSR, XeSS selon le matériel) et pourcentage de résolution | Oui | — |
| Génération de trames | Oui (si plugin intégré) | — |
| Lumen matériel (ray tracing) | Oui (si GPU compatible) | — |
| Limite de fps (`t.MaxFPS`), V-Sync | Oui | 40 Hz optionnel (écrans 120 Hz) |
| Flou de mouvement, profondeur de champ, grain | Oui | Oui (confort) |
| Champ de vision | Oui | Oui (borné) |

- **Détection automatique** au premier lancement sur PC (benchmark de matériel du moteur), puis réglage manuel.
- Les changements de textures et de distances de vue peuvent nécessiter un rechargement : à signaler dans l'interface.
- Les options n'ont **aucun effet sur le gameplay** (visibilité des ennemis, végétation masquant un joueur) : la végétation qui sert de couvert est conservée à tous les niveaux.

---

## 16. Méthode : profilage continu et critères d'acceptation

### 16.1 Scènes de référence (rejouées automatiquement à chaque build nocturne)

| N° | Scène | Ce qu'elle stresse |
|---|---|---|
| 1 | Traversée du monde à vitesse max (caméra scriptée, 3 biomes, jour → nuit) | Streaming WP, HLOD, textures, PSO, VSM sur rotation du soleil |
| 2 | Forêt dense + orage + 10 créatures en combat | Végétation, WPO, Lumen, Niagara, animation, IA |
| 3 | Base de 3 000 pièces + destruction de la fondation centrale | ISM, intégrité, effondrement, débris, réseau |
| 4 | Combat du dragon, 4 joueurs, phases 1 → 3 (bots) | Niagara des souffles, zones de feu, Data Layers d'arène, Geometry Collections, réseau |
| 5 | 4 joueurs dispersés aux 4 coins de la carte (listen server) | Streaming serveur, mémoire de l'hôte, game thread de l'hôte |

Chaque passe produit un rapport CSV (CSV Profiler + PerfReportTool) : frame time moyen, 1 % et 0,1 % bas, ms par thread, ms GPU par poste, mémoire, saccades > 50 ms, débit réseau. Les rapports sont comparés au build précédent : **une régression > 10 % sur un indicateur ouvre un ticket automatique**.

### 16.2 Critères d'acceptation par jalon

| Jalon | Plateformes mesurées | Critères (toutes cibles du §1 appliquées aux scènes listées) |
|---|---|---|
| **Vertical Slice** | PC moyen, PS5 **et Series S** | Scènes 2 et 4 : budgets tenus à +25 % ; décision Series S option A/B (§5.5) ; décision ADR réplication des pièces (§13.4) et risque listen-server Series S (§8.6) ; PSO precaching activé |
| **Alpha** | Toutes | Scènes 1 à 4 : budgets à +15 % ; zéro chargement synchrone en exploration ; HLOD générés en pipeline ; pools en place |
| **Beta** | Toutes | Scènes 1 à 5 : budgets tenus (marge 10 % incluse) ; 1 % bas ≥ 90 % du fps cible ; aucune saccade PSO > 50 ms sur PC ; mémoire Series S ≤ 7,5 Go en scène 5 |
| **Release candidate** | Toutes, matériel de détail (pas seulement les kits) | Beta + 4 h de session coop sans fuite mémoire (dérive < 100 Mo/h, cible) ; exigences de certification consoles |

### 16.3 Organisation

- **Un référent performance** par discipline (rendu, gameplay/IA, réseau, contenu), revue hebdomadaire des rapports.
- **Budgets par équipe** (ms et Mo) : chaque discipline possède sa ligne du §1.3/§1.4 ; un dépassement se négocie, ne se subit pas.
- **Règle de contenu** : tout nouvel asset lourd (créature, biome, effet de boss) est mesuré dans une scène de test avant intégration dans le monde.

---

## 17. Checklist

### Rendu
- [ ] Profils de plateforme (Device Profiles + `DefaultScalability.ini`) créés pour les 6 profils du §1.1
- [ ] Nanite activé sur tout le statique opaque ; translucide séparé ; repli (fallback) réglé
- [ ] Décision feuillage (Nanite masqué vs non Nanite) prise par mesure A/B
- [ ] « WPO Disable Distance » et invalidation de cache d'ombre réglés sur toute la végétation
- [ ] Rotation du soleil par paliers ; coût VSM mesuré
- [ ] Lumen : Mesh Distance Fields vérifiés, murs ≥ 10 cm, pièces de construction compatibles
- [ ] Décision Series S (option A ou B) au Vertical Slice
- [ ] Cull distances sur tout le non Nanite ; densité de végétation par profil

### Monde
- [ ] World Partition : grilles et rayons du §6.2 ; OFPA ; contrôle de version adapté
- [ ] HLOD en pipeline automatique ; acteurs dynamiques exclus
- [ ] Data Layers d'arène préchargés (Loaded) avant le combat ; test « client qui rejoint en P3 »
- [ ] Streaming serveur configuré et testé (scène 5)

### Contenu et mémoire
- [ ] LOD Groups de textures plafonnés par plateforme ; compression BC correcte ; Oodle Texture activé
- [ ] RVT terrain en place ; pools VT dimensionnés
- [ ] `stat streaming` sans dépassement dans les scènes de référence
- [ ] Classes hub sans référence dure vers du contenu lourd (Size Map vérifiée)
- [ ] Bundle `Boss` préchargé à l'approche de la caldera

### Gameplay
- [ ] Pools : flèches, projectiles, zones de feu, débris, lumières, décalques, Niagara
- [ ] Significance Manager : paliers S0-S3 ; IA dormante
- [ ] URO, `VisibilityBasedAnimTickOption`, Animation Budget Allocator configurés ; notifies de coup vérifiés sur serveur dédié
- [ ] Navmesh dynamique avec invoqueurs ; coût mesuré

### Réseau
- [ ] Push model activé ; fréquences et distances du §13.2 appliquées
- [ ] Bases répliquées en Fast Array, dormantes ; débris non répliqués
- [ ] Bande passante mesurée en scène 4 (pic ≤ 80 Ko/s par client)

### Construction
- [ ] ISM Nanite par tronçon ; limites du §14.2 en configuration
- [ ] Intégrité incrémentale étalée (0,5 ms/frame) ; effondrement à 32 pièces par vague (0,08 s)
- [ ] Test de charge scène 3 passé sur toutes les plateformes

### Méthode
- [ ] Passe automatique nocturne (5 scènes, CSV, comparaison)
- [ ] Collecte PSO automatisée sur chaque build de jalon
- [ ] Critères du jalon courant validés et rapport archivé

---

## 18. Points ouverts

| # | Question | Qui tranche | Échéance |
|---|---|---|---|
| Q1 | Taille réelle du monde (hypothèse 8 × 8 km) | Chef d'équipe / GDD | Avant le Vertical Slice |
| Q2 | Serveurs dédiés proposés ? (impacte §6.5, §8.6, §13.2 et le coût d'exploitation récurrent) | Production | Vertical Slice |
| Q3 | Series S : option A (Lumen bas) ou B (repli) | Rendu + DA | Vertical Slice |
| Q4 | Réplication des pièces en Fast Array par base (modifie l'usage de `AEMBBuildablePiece` prévu au contrat §2) | Chef d'équipe + Dev B | Avant implémentation réseau de la construction |
| Q5 | Version moteur figée (5.4 ou plus récente) : conditionne Nanite Foliage, Nanite squelettique, statut d'Iris, comportement de streaming serveur | Direction technique | Avant production |
| Q6 | Validation serveur des fenêtres de parade (10 frames) sous latence, avec un serveur à 30 Hz | Dev A | Vertical Slice |
| Q7 | Matériel PC minimum et recommandé officiel | Production | Alpha |
