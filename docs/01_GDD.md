# Emberfall — Game Design Document

> Version 1.1 — 23/10/2026 — nom de code « Emberfall » (modifiable).
> Genre : survie / action-RPG à la troisième personne, dark fantasy médiévale réaliste. Références : Elden Ring, Dark Souls III (combat, boss), ARK et Valheim (survie, construction, engrammes), Dragon's Dogma 2 (créatures colossales).
> Moteur : Unreal Engine 5.4+ (C++). Coop 1 à 4 joueurs, serveur autoritaire.
> **Toutes les valeurs chiffrées viennent de `00_Constantes_et_Contrats.md`**, qui fait foi. Ce sont des valeurs de conception à équilibrer en playtest : aucune n'a été mesurée.

## Sommaire
1. [Vision et piliers](#1-vision-et-piliers)
2. [Direction artistique et ambiance](#2-direction-artistique-et-ambiance)
3. [Boucles de gameplay](#3-boucles-de-gameplay)
4. [Survie et récolte](#4-survie-et-récolte)
5. [Construction](#5-construction)
6. [Combat souls-like](#6-combat-souls-like)
7. [Bestiaire](#7-bestiaire)
8. [Progression : Skill Tree, Knowledge Tree, Glyphes](#8-progression)
9. [Boss](#9-boss)
10. [Coop](#10-coop)
11. [Questions ouvertes](#11-questions-ouvertes)

---

## 1. Vision et piliers

**Pitch.** Le monde de Cendrehaute brûle encore de la chute des dragons ancestraux. Les survivants fouillent ses ruines, bâtissent des refuges contre la Corruption qui monte la nuit, et réapprennent, glyphe après glyphe, les savoirs perdus qui permettront d'affronter Vaelthyrax, l'Ancêtre-Braise.

| Pilier | Ce qu'il implique |
|---|---|
| **Chaque coup se lit** | Télégraphes lisibles, fenêtres chiffrées, aucune attaque sans contre-jeu. La mort vient d'une erreur du joueur, jamais de l'aléatoire. |
| **Le monde résiste** | Survie qui pèse (faim, soif, froid, Corruption) sans devenir une corvée ; matière physique (arbres qui tombent, roches qui éclatent, bases qui s'effondrent). |
| **Le savoir est le vrai butin** | La progression la plus forte vient des glyphes : technologies, plans, secrets trouvés en explorant et en vainquant les boss. |
| **Bâtir pour durer** | La base est un outil de progression (ateliers, stockage, purification) et un enjeu (raids des dragons territoriaux). |

---

## 2. Direction artistique et ambiance

### 2.1 Intention
Dark fantasy **réaliste et matérielle** : pierre humide, fer piqué, cuir craquelé, cendre en suspension. Pas de surenchère de couleurs saturées : la lumière raconte. Le feu (chaleur, sécurité, mais aussi la menace draconique) est la seule couleur chaude franche ; la Corruption est froide, violacée et désaturée.

### 2.2 Palette
| Usage | Teintes | Notes |
|---|---|---|
| Monde de jour | gris basalte, ocres terreux, verts mousse éteints | contraste modéré, ciel voilé |
| Feu, braise, dragons | orange braise, rouge sang séché, or terni | seule zone de saturation forte, guide l'œil (feux de camp, gorge du dragon) |
| Corruption | violet sombre, bleu nuit, cyan malade en émissif | désaturation de l'image par post-process proportionnelle à la jauge de Corruption |
| Interface | parchemin sombre, encre, filets or terni | sobre, diégétique quand c'est possible |

### 2.3 Rendu (UE5)
- **Lumen** : illumination globale et réflexions dynamiques, indispensables au cycle jour/nuit complet et aux sources de feu mobiles (torches, souffles du dragon, incendies). Réglages par plateforme dans `03_Scalabilite.md`.
- **Nanite** : falaises, rochers, ruines, architecture et pièces de construction en haute densité. La végétation animée (World Position Offset) et les meshes squelettiques restent en LOD classiques (limites détaillées dans `03_Scalabilite.md`).
- **PBR 4K** : textures 4K pour les héros (personnages, boss, armes) et le premier plan ; Virtual Textures pour le terrain (RVT) et streaming pour le reste. Matériaux en couches (mousse, neige, cendre, humidité) pilotés par la météo.
- **Virtual Shadow Maps** pour des ombres nettes et stables à toutes les distances.
- **Niagara** : cendres en suspension, braises, souffles, effondrements, pluie.
- **Chaos** : chutes d'arbres, roches fracturables (Geometry Collections), effondrement des bases et des arènes.

### 2.4 Cycle jour/nuit et météo dynamique
- **1 journée = 48 minutes réelles** : jour 32 min, nuit 16 min. La nuit est plus courte mais dangereuse : brume corrompue, Traqueurs de brume, Corruption ambiante ×2.
- **États météo** : Clair, Couvert, Pluie, Orage, Brouillard, Neige (biomes froids), Brume corrompue (biome du Marais flétri et la nuit), Tempête de cendres (Caldera).
- **Transitions** de 90 à 180 s (interpolation des paramètres du ciel, du vent, des matériaux mouillés/enneigés, du brouillard volumétrique).
- **Effets de gameplay** : la pluie éteint les feux non abrités et rafraîchit (−4 °C) ; l'orage attire Tempestar (dragon territorial) ; la neige ralentit (−10 % de vitesse hors chemins) ; le brouillard réduit la perception des créatures (et du joueur) de 40 %.

### 2.5 Références visuelles (à rassembler en moodboard par le directeur artistique)
Elden Ring (Caelid, Leyndell en cendres), Dark Souls III (Irithyll, Lothric), Bloodborne (verticalité gothique), A Plague Tale: Requiem (lumière naturelle), peintures de John Martin (apocalypses), Zdzisław Beksiński (Corruption), gravures de Gustave Doré.

### 2.6 Sound design
- **Mixage par proximité et menace** : bus dédiés (ambiance, créatures, combat, interface, musique) ; le mix baisse l'ambiance quand un boss télégraphie.
- **Télégraphes sonores obligatoires** : chaque attaque de boss a une signature audio distincte (inspiration du souffle, raclement de griffes, sifflement de la queue) jouée au début du télégraphe. Accessibilité : option de sous-titres d'attaque.
- **Ambiances** : vent dans les ruines, craquements de bois des bases (plus forts quand l'intégrité < 25), chuchotements quand la Corruption dépasse 25.
- **Musique** : quasi absente en exploration (nappes et drones), cordes graves et chœurs pour les boss, un thème par phase (P3 du dragon : percussion et chœur à pleine puissance).
- **Physique** : bruits d'impact selon le matériau (bois, pierre, fer, os), chute d'arbre avec craquement anticipé (2 s) pour prévenir les joueurs dessous.

---

## 3. Boucles de gameplay

### 3.1 Boucle à la minute
Explorer → repérer (ressource, créature, ruine) → récolter ou combattre → gérer ses jauges (endurance en combat, faim/soif/température/Corruption) → rentrer sous un abri ou un feu avant la nuit.

### 3.2 Boucle à l'heure
Préparer une expédition (nourriture, fioles, outils, armes) → atteindre un nouveau biome ou un donjon → récupérer un savoir (tome, inscription, lieu secret) et des ressources rares → rentrer à la base → forger, construire, débloquer des glyphes → affronter un ennemi plus dur.

### 3.3 Boucle de campagne (≈ 60-80 h)
| Acte | Niveaux | Objectif | Jalon |
|---|---|---|---|
| I. Les Cendres froides | 1-22 | Survivre, outils de silex, bois puis pierre | **Colosse d'Ossivaire** |
| II. Le Clocher flétri | 22-48 | Fer, acier noir, magie primordiale, Marais flétri | **Nyxariel, l'Aile Flétrie** |
| III. Les Dragons de Cendrehaute | 48-75 | Forge draconique, dragons territoriaux, architecture draconique | 3 dragons territoriaux (optionnels mais conseillés) |
| IV. La Caldera | 75-80 | Purifier la Caldera | **Vaelthyrax, l'Ancêtre-Braise** |
| NG+ | 80 | Monde réinitialisé, ennemis ×1,5, glyphe secret « Cœur-Braise lié » | — |

---

## 4. Survie et récolte

### 4.1 Ressources
| Identifiant (code) | Nom | Source | Outil minimal | Poids |
|---|---|---|---|---|
| `Wood` | Bois | arbres (Chaos), branches | aucun (branches) / hache de silex | 0,5 |
| `Fiber` | Fibre | buissons, herbes | aucun | 0,1 |
| `Flint` | Silex | sol rocailleux | aucun | 0,5 |
| `Stone` | Pierre | rochers fracturables | pioche de silex | 1 |
| `IronOre` | Minerai de fer | filons (falaises) | pioche de fer… (silex : rendement ×0,3) | 2 |
| `IronIngot` | Lingot de fer | fonderie | — | 1 |
| `BlackSteelIngot` | Lingot d'acier noir | fonderie + charbon + savoir | — | 1 |
| `Obsidian` | Obsidienne | coulées de la Caldera, failles | pioche d'acier noir | 1,5 |
| `MoonSilver` | Argent-lune | filons nocturnes (visibles de nuit seulement) | pioche d'acier noir | 1 |
| `DragonScale` | Écaille draconique | dracosaures, dragons | couteau de dépeçage | 2 |
| `DragonBone` | Os draconique | dragons territoriaux, carcasses anciennes | hache d'acier noir | 3 |
| `EmberHeart` | Cœur-Braise | dragons territoriaux (1 garanti), Vaelthyrax | — | 5 |

**Charge portée** : maximum 300 (v1 : ressources seulement ; l'équipement s'ajoutera). Le ratio de charge règle la roulade (voir §6.4).

### 4.2 Outils et tiers
| Tier | Matériau | Outils | Rendement | Débloqué par |
|---|---|---|---|---|
| T1 | Silex / bois | hache, pioche, couteau, torche | ×1,0 | glyphe `G_StoneTools` (niv. 1) |
| T2 | Fer | hache, pioche, faucille, marteau de construction | ×1,6, durabilité ×2 | `G_Smelting` (niv. 10) |
| T3 | Acier noir | outils et pioche « fend-roc » | ×2,2, peut miner Obsidienne et Argent-lune | `G_BlackSteel` (niv. 20, savoir de la Bibliothèque engloutie) |
| T4 | Draconique | outils de croc et d'os | ×3,0, dépeçage des dragons à 100 % | `G_DragonboneWeapons` (niv. 60, après la Forge draconique) |

### 4.3 Interactions physiques
- **Arbres (Chaos)** : un arbre a des PV (pin 300, chêne 600, arbre-cendre 1 200). À 0, l'arbre devient un corps physique et tombe dans la direction opposée au dernier coup ; il inflige 150 dégâts (poise 60) à ce qu'il écrase. Le tronc au sol se découpe en 3 rondins (récolte ×1,5). Coût : simulation limitée à 3 arbres simultanés par joueur, puis passage en « sommeil » physique et remplacement par un mesh statique après 10 s.
- **Rochers fracturables** : Geometry Collections à 2 niveaux de fracture ; chaque fragment détaché donne de la pierre. Les débris sont poolés et disparaissent après 15 s (voir `03_Scalabilite.md`).
- **Filons** : non fracturables (performance), ils s'épuisent puis repoussent en 2 jours de jeu.

### 4.4 Jauges de survie
| Jauge | Plage | Baisse | Seuils et effets | Remèdes |
|---|---|---|---|---|
| **Faim** | 0-100 | −1,6 / min réelle (≈ 62 min) ; ×1,5 par grand froid | < 25 : régénération d'endurance −30 % ; 0 : −1 PV/s | nourriture cuite (+20 à +60), viande crue (+10, risque d'intoxication) |
| **Soif** | 0-100 | −2,5 / min (40 min) ; ×2 au-dessus de 35 °C | < 25 : endurance max −20 % ; 0 : −2 PV/s | eau bouillie (+40), outre (+25 × 4), pluie |
| **Température** | ressentie, en °C | dépend du biome, de l'heure, de la météo, des vêtements (isolation) | confort 10-28 °C ; < 5 °C : faim ×1,5 ; < −10 °C : −1 PV/s ; > 35 °C : soif ×2, régén. d'endurance −20 % ; > 45 °C : −1 PV/s | feu (+15 °C dans 6 m), fourrures, huile ignifuge (Caldera) |
| **Corruption** | 0-100 | +0,5/s dans la brume corrompue ; +2 à +20 par coup corrompu ; nuit ×2 | 25 : chuchotements, hallucinations sonores ; 50 : PV max −15 % ; 75 : PV max −30 %, apparition d'ennemis spectraux ; 100 : mort « Possédé » (le cadavre devient un ennemi à vaincre pour récupérer son sac) | brasero purificateur (−1/s dans 8 m), tonique d'épuration (−30), dormir dans un lit abrité (−50) |

---

## 5. Construction

### 5.1 Principes
- **Grille modulaire de 300 cm**, pivot centre-bas. Pas d'accrochage par catégorie : fondation, sol, toit 300 cm ; mur, pilier, escalier 150 cm ; mobilier et ateliers 25 cm. Le serveur revérifie que la pièce est sur la grille de sa catégorie (±5 cm) ou sur un socket.
- **Sockets** (`Snap_<Type>_<Suffixe>` sur les meshes) : accrochage au socket compatible le plus proche dans un rayon de 150 cm, sinon à la grille.
- **Rotation** par pas de 90° (structure) ou 15° (mobilier).
- **Aperçu fantôme** : vert (intégrité prévue > 50), jaune (25-50), orange (1-24), rouge (placement invalide, raison affichée : trop loin, chevauchement, pas de support, pente trop forte, ressources insuffisantes, zone interdite, intégrité insuffisante, limite atteinte).
- **Règles de placement** : 800 cm maximum du personnage (950 tolérés côté serveur pour la latence) ; chevauchement testé par une boîte réduite de 5 % ; fondations : support au sol par traces (jusqu'à 180 cm d'enfoncement) et pente maximale 30° ; zones interdites (arènes de boss, villages, donjons).
- **Limites** : 3 000 pièces par base (ensemble connecté), 15 000 par monde.

### 5.2 Pièces
| Catégorie | Pièces | Dimensions | Rôle structurel |
|---|---|---|---|
| Fondation | carrée, triangulaire | 300 × 300 × 50 | ancrée au sol : intégrité 100 |
| Sol / plafond | carré, triangle | 300 × 300 × 20 | transmet horizontalement |
| Mur | plein, demi-mur, porte, fenêtre, arche | 300 × 20 × 300 | transmet verticalement |
| Pilier | pilier | 30 × 30 × 300 | décroissance verticale ×0,5 |
| Toit | pente 45°, faîtage, coin | 300 × 424 | transmet horizontalement |
| Escalier | droit, demi-tour | 300 × 300 × 300 | transmet verticalement |
| Mobilier | lit, coffre, table, râtelier, bannière | libre | feuille (ne soutient rien) |
| Ateliers | feu de camp, établi, forge, fonderie, alambic, table d'alchimie, forge draconique | libre | feuille ; débloqués par glyphes |

Données complètes : `Data/DT_BuildPieces.csv` (struct `FEMBBuildPieceRow`).

### 5.3 Intégrité structurelle
Intégrité d'une pièce = **maximum** sur ses supports de (intégrité du support − décroissance de **son propre** matériau). Liaison verticale si la pièce est au-dessus de son support (Δz > 50 cm), horizontale sinon. Fondation ancrée = 100 ; une pièce à 0 ou moins s'effondre.

| Matériau | Tier | PV de la pièce | Décroissance verticale | Décroissance horizontale | Portée depuis une fondation |
|---|---|---|---|---|---|
| Bois | T1 | 500 | 10 | 25 | 9 niveaux / 3 travées en porte-à-faux |
| Pierre | T2 | 1 500 | 6 | 20 | 16 niveaux / 4 travées |
| Fer forgé | T3 | 3 000 | 5 | 12 | 19 niveaux / 8 travées |
| Draconique | T4 | 6 000 | 3 | 8 | 33 niveaux / 12 travées |

Exemple : une tour de murs en bois sur fondation donne 90, 80, 70… 10 ; le 10e mur serait à 0, il est refusé. Un pilier en pierre ne perd que 3 par niveau.

**Effondrement en cascade** : quand une pièce est détruite, le serveur recalcule l'intégrité du sous-ensemble connecté (algorithme de Dijkstra « plus large chemin », `UEMBStructureSubsystem`). Les pièces passées à 0 tombent **par niveaux** : une vague toutes les 0,08 s, 32 pièces maximum par vague, les plus proches du point de rupture d'abord. Les 8 premières pièces de chaque vague produisent des débris physiques (Chaos), les autres un nuage de poussière.

**Coûts** (mur standard) : bois 10 Wood ; pierre 12 Stone + 2 Wood ; fer 6 IronIngot + 6 Stone ; draconique 2 DragonBone + 8 Obsidian + 4 IronIngot. Fondation ×2, sol/toit/escalier ×1, pilier ×0,5 (arrondi supérieur).

**Dégâts aux pièces** : les pièces reçoivent les coups (`IEMBHitReceiver`) : physique ×1,0 ; feu ×1,5 sur le bois, ×0,5 sur les autres matériaux ; autres types ×0,5. Les dragons territoriaux et les créatures ciblent les bases (§7.3) ; en coop PvE, les armes des joueurs n'abîment pas les constructions (option serveur réservée à un futur mode PvP).

---

## 6. Combat souls-like

### 6.1 Principes
Endurance comme ressource centrale, engagement (une action lancée va au bout sauf annulation prévue), lecture des télégraphes, punition des erreurs (counter-hit), récompense de la maîtrise (parade parfaite, riposte critique, fenêtres de vulnérabilité des boss).

### 6.2 Attributs de base (niveau 1)
PV 500 · Endurance 100 · Poise 50. Régénération d'endurance 30/s après 0,8 s sans consommation (1,5 s si la jauge a été vidée), ×0,35 en garde. Régénération de poise 15/s après 2 s sans coup reçu. Une action est autorisée tant que l'endurance est > 0 (elle peut finir à 0). Sprint : 8/s ; saut : 10. Buffer d'input : 0,2 s (12 frames).

### 6.3 Tableau des actions (arme de référence : épée longue, 80 dégâts de base, 60 fps)
| Action | Endurance | Dégâts | Poise infligée | Durée | Fenêtres (frames) | Notes |
|---|---|---|---|---|---|---|
| Légère (combo ×3) | 14 | 80 / 80 / 96 | 12 | 0,60 s (36 f) | startup 0-11, actif 12-17, recovery 18-35, **combo 18-30** | enchaînement dans la fenêtre ; buffer 0,2 s |
| Lourde | 26 | 116 | 25 | 1,00 s (60 f) | startup 0-23, actif 24-31, recovery 32-59 | **hyper-armure** frames 6-31 (poise subie ×0,5) |
| Chargée | 38 au relâchement | 128 → 192 | 35 → 55 | charge 0,4-1,2 s + frappe 0,9 s | relâché < 0,4 s → lourde ; **relâchement automatique à 1,2 s** | hyper-armure pendant charge et actif ; annulable par roulade pendant la charge ; la charge compte comme startup |
| Roulade | 18 (22 en charge lourde) | — | — | 0,70 s (42 f) | **i-frames 3-15 (13 f = 0,217 s)** ; annulable dès la frame 30 | i-frames 11 f en charge moyenne, 9 f en charge lourde |
| Parade | 10 | — | 35 de posture à l'attaquant | 0,75 s (45 f) | **parfaite 4-13 (10 f = 0,167 s)** ; ratée : vulnérable 14-44 | parade parfaite = 0 dégât, ouvre une **riposte ×3** pendant 1,5 s |
| Garde | coût par coup = endurance de l'attaque (sinon dégâts × 0,5) | réduction : physique 85 %, élémentaire 50 %, corruption 30 % | — | levée 0,10 s (6 f) | régénération d'endurance ×0,35 | endurance à 0 → **rupture de garde** : étourdi 1,2 s (72 f) |

### 6.4 Charge portée et roulade
| Charge | Ratio | I-frames | Coût |
|---|---|---|---|
| Légère | < 30 % | 13 f (3-15) | 18 |
| Moyenne | 30-70 % | 11 f (3-13) | 18 |
| Lourde | 70-100 % | 9 f (3-11) | 22 |
| Surchargé | > 100 % | pas de roulade | — |

### 6.5 Poise, posture et punitions
| Règle | Valeur |
|---|---|
| Poise brisée (joueur) | étourdi 0,8 s (48 f), poise remise au maximum |
| Counter-hit (coup reçu en recovery, en startup ou en charge, ou après une parade ratée) | dégâts reçus ×1,25 |
| Hyper-armure | poise subie ×0,5 |
| Posture des boss | jauge = poise du boss ; brisée → étourdi 4,0 s, riposte critique ×3 |
| Cumul des critiques | la riposte ×3 après parade ne s'ajoute pas au critique ×3 d'un boss étourdi (pas de ×9) ; points faibles et vulnérabilité se multiplient |
| Dégâts sur la durée (lave, brûlure, zones de feu) | ni esquivables par i-frames, ni parables, ni majorés par counter-hit |

### 6.6 Lecture des fenêtres (exemple)
Le dragon mord (télégraphe 0,9 s, fenêtre d'esquive 0,80-1,05 s depuis le début). La roulade donne 0,217 s d'invulnérabilité à partir de 0,05 s après l'appui : il faut appuyer entre ≈ 0,75 et 0,95 s pour couvrir l'impact. La morsure est la seule attaque parable du dragon : une parade lancée à ≈ 0,83 s place la fenêtre parfaite (0,067-0,233 s) sur l'impact.

---

## 7. Bestiaire

Statistiques au niveau de référence de la zone ; les PV sont multipliés par 1,5 en NG+.

### 7.1 Créatures corrompues
| Créature | Zone / niveau | PV | Dégâts | Poise | Comportement | Butin |
|---|---|---|---|---|---|---|
| Goule de cendre | Cendres froides, 1-8 | 180 | 35 | 20 | en groupe de 2-4, charge maladroite, fuit le feu | fibre, os, ration avariée |
| Loup-fléau | forêts, 5-15 | 220 | 40 (+5 Corruption) | 25 | meute de 3-5, encerclement, attaque par le flanc quand le joueur garde | cuir, viande |
| Moine flétri | ruines, 10-20 | 260 | 60 (sort à distance) | 30 | porte une lanterne qui soigne les corrompus proches (10 PV/s) : à tuer en premier | parchemins (savoirs), huile |
| Chevalier creux | ruines, donjons, 15-35 | 650 | 90 | 80 | bouclier (garde frontale 80 %), **parable**, punit les attaques lourdes répétées | fer, armes de fer |
| Ronce-mère | Marais flétri, 25-40 | 1 500 (immobile) | 50 (racines) | ∞ | génère des goules toutes les 20 s, vulnérable au feu ×2 | fibre corrompue, `Lore_AshRites` |
| Traqueur de brume | partout la nuit, 20-60 | 400 | 120 | 40 | invisible dans la brume, visible à la torche ou au feu, attaque les joueurs isolés | essence de brume (alchimie) |

### 7.2 Dracosaures mineurs
| Créature | Niveau | PV | Attaques | Poise | Comportement | Butin |
|---|---|---|---|---|---|---|
| Wyverneau des rocs | 18-30 | 900 | morsure 60, piqué bas 80 | 60 | vol bas, harcèle, fuit à 30 % PV | DragonScale ×1-2 |
| Drake de braise | 28-45 | 1 400 | souffle court (3 s, 25/tick, 8 ticks/s), queue 90 | 90 | gardien de filons d'obsidienne, enflamme les bases en bois | DragonScale ×2, Obsidian |
| Basilic des marais | 30-45 | 1 800 | regard (cône 40°, 40 de poise/s : brise la garde), morsure 110 | 120 | embuscade dans l'eau ; se casser la ligne de vue | DragonScale ×2, glande (alchimie) |
| Dracosaure cuirassé | 40-60 | 3 000 | charge 180, coup de queue 150 | 200 | carapace frontale (−80 % dégâts), ventre exposé après une charge ratée (3 s) | DragonScale ×3, DragonBone ×1 |

### 7.3 Dragons sauvages territoriaux (mini-boss du monde)
Chacun possède un **territoire** (rayon 300 m autour de son nid) : rugissement d'avertissement à l'entrée, attaque si le joueur reste plus de 20 s. **Raids** : toutes les 3 journées de jeu, s'il existe une base de plus de 50 pièces à moins de 200 m de son territoire, le dragon l'attaque (priorité aux ateliers). Un brasero purificateur actif réduit la probabilité de raid de 50 %.

| Dragon | Biome | Niveau | PV | Posture | Élément | Signatures | Butin |
|---|---|---|---|---|---|---|---|
| Sylvadrac | forêt profonde | 50 | 25 000 | 800 | poison (dégâts sur la durée) | nuage toxique persistant, morsure, piqué bas | EmberHeart ×1, DragonBone ×4, DragonScale ×8 |
| Glacivorn | Crêtes blanches | 58 | 30 000 | 1 000 | givre (gel : roulade ×1,3 plus lente 5 s) | souffle de givre, avalanche déclenchée, murs de glace | EmberHeart ×1, DragonBone ×5, DragonScale ×10 |
| Tempestar | côtes orageuses | 65 | 35 000 | 1 100 | foudre | n'attaque que sous l'orage, éclairs ciblés (télégraphe 1,5 s), piqué | EmberHeart ×1, DragonBone ×6, MoonSilver ×10 |

---

## 8. Progression

### 8.1 Niveaux et points
- Niveau maximum : **80**. L'XP vient des combats, de la récolte, de l'artisanat et des découvertes. XP pour passer du niveau L à L+1 : `round(150 × L^1,5)` (≈ 3,38 millions d'XP cumulés jusqu'au niveau 80).
- **Points d'Éveil** (Skill Tree) : 1 par niveau gagné (79) + 1 par boss majeur (3) = **82**.
- **Points de Glyphe** (Knowledge Tree) : niveaux 2-20 : 4 par niveau ; 21-50 : 6 ; 51-80 : 8. Total au niveau 80 : 19×4 + 30×6 + 30×8 = **496**.
- Dans l'arbre final, le coût total des glyphes (≈ 700 points visés) dépassera ce total : le joueur devra **choisir une spécialisation**, ce qui rend la coop complémentaire (voir §8.4 pour le jeu de départ).

### 8.2 Skill Tree (Points d'Éveil)
Trois branches ; chaque nœud a un niveau requis, des parents et un rang maximum. Actif = compétence équipée sur un raccourci (3 emplacements), passif = permanent.

| Branche | Nœud | Type | Niv. | Rangs × coût | Effet |
|---|---|---|---|---|---|
| Maîtrise des armes | Poigne d'acier | passif | 2 | 3 × 1 | dégâts physiques +4 % par rang |
| | Souffle du duelliste | passif | 6 | 3 × 1 | coût en endurance des attaques −5 % par rang |
| | Riposte sanglante | passif | 14 | 1 × 2 | riposte critique ×3 → ×3,5 |
| | Pas fantôme | passif | 20 | 1 × 3 | +2 frames d'i-frames sur la roulade |
| | Tempête d'acier | actif | 25 | 1 × 3 | 4 coups rapides, 25 endurance, recharge 20 s |
| | Garde inébranlable | passif | 30 | 2 × 2 | coût de garde −15 % par rang |
| | Parade du maître | passif | 40 | 1 × 4 | fenêtre de parade parfaite 10 → 12 frames |
| Survie | Estomac de fer | passif | 2 | 3 × 1 | baisse de faim et de soif −10 % par rang |
| | Récolteur | passif | 4 | 3 × 1 | rendement de récolte +10 % par rang |
| | Peau tannée | passif | 10 | 2 × 1 | zone de confort thermique élargie de 3 °C par rang |
| | Bâtisseur | passif | 12 | 2 × 2 | coût des pièces −10 % par rang |
| | Bête de somme | passif | 16 | 3 × 1 | charge maximale +30 par rang |
| | Second souffle | actif | 22 | 1 × 2 | rend 50 % d'endurance instantanément, recharge 60 s |
| | Volonté pure | passif | 35 | 2 × 2 | gain de Corruption −20 % par rang |
| Magies primordiales | Étincelle | actif | 8 | 1 × 1 | projectile de feu 60 dégâts, 15 endurance |
| | Lame embrasée | actif | 18 | 1 × 2 | arme enflammée 20 s (+30 feu par coup), recharge 45 s |
| | Mur de cendre | actif | 26 | 1 × 2 | mur qui bloque les projectiles 6 s |
| | Glyphe de givre | actif | 32 | 1 × 3 | piège au sol, gèle (poise 80) |
| | Résonance de foudre | actif | 42 | 1 × 3 | chaîne d'éclairs (3 cibles, 120) |
| | Communion draconique | passif | 55 | 1 × 4 | résistance feu +25 %, requiert le savoir `Lore_DraconicTongue` |

Données : struct `FEMBSkillNodeRow` (`EMBProgressionTypes.h`).

### 8.3 Knowledge Tree et système d'Engrammes/Glyphes
Le Knowledge Tree regroupe les **technologies, plans et recettes**, chacun représenté par un **Glyphe** (équivalent des engrammes d'ARK). Cinq arbres : Survie, Construction, Forge, Alchimie, Arcanes.

**Prérequis d'un glyphe** (tous cumulatifs) :
1. **Niveau requis** (`RequiredLevel`).
2. **Glyphes parents** (`ParentGlyphs`) déjà appris.
3. **Savoirs requis** (`RequiredKnowledge`) : fragments de lore à trouver dans le monde (tomes, inscriptions, cadavres de savants). Exemples : `Lore_AncientScript` (ruines du Val Gris), `Lore_AshRites` (Ronce-mère), `Lore_SunkenLibraryTreatise` (Bibliothèque engloutie), `Lore_DraconicTongue` (stèle du nid de Sylvadrac).
4. **Déclencheur secret** (`SecretTrigger`, Gameplay Tag) : boss vaincu (`Event.Boss.Killed.*`) ou lieu découvert (`Event.Location.Discovered.*`).
5. **Coût** en points de glyphe (`Cost`).

**Glyphes secrets** (`bHidden = True`) : invisibles dans l'arbre (case « ??? ») tant que leur déclencheur n'est pas obtenu ; une fois révélés, ils restent à acheter avec des points. En coop, le déclencheur d'un boss vaincu vaut pour tous les joueurs présents ; une découverte de lieu ne vaut que pour celui qui l'a faite.

**Règles d'interface** : le glyphe affiche ses prérequis manquants en rouge ; le reset complet coûte un objet rare (Larme d'oubli, 1 par boss).

### 8.4 Table de données — extrait de `Data/DT_Glyphs.csv`
Struct C++ `FEMBGlyphRow` (`EMBProgressionTypes.h`), import direct en DataTable dans l'éditeur. Les tags accordés (`Glyph.*`) sont déclarés dans `Config/Tags/EmberfallGlyphs.ini`.

```csv
---,DisplayName,Tree,Tier,Cost,RequiredLevel,ParentGlyphs,RequiredKnowledge,SecretTrigger,bHidden,GrantedTag,Description
G_StoneTools,"Outils de silex",Survival,1,2,1,(),(),"(TagName="""")",False,"(TagName=""Glyph.StoneTools"")","Hache, pioche et couteau de silex."
G_Smelting,"Fonte du fer",Forge,2,10,10,(G_Forge),(),"(TagName="""")",False,"(TagName=""Glyph.Smelting"")","Fonderie, lingots de fer et outils T2."
G_DraconicForge,"Forge draconique",Forge,4,30,55,(G_BlackSteel),(Lore_DraconicTongue),"(TagName=""Event.Boss.Killed.Seraph"")",False,"(TagName=""Glyph.DraconicForge"")","Travail de l'os et de l'écaille de dragon. Exige la mort de Nyxariel."
S_StarWellAttunement,"Accord du Puits-d'Étoiles",Arcana,3,18,35,(G_RuneReading),(),"(TagName=""Event.Location.Discovered.StarWell"")",True,"(TagName=""Glyph.StarWellAttunement"")","Glyphe secret : régénération d'endurance +10 % la nuit."
```
Formats d'import UE : tableaux `(A,B)` (entre guillemets CSV s'ils contiennent une virgule), tags `(TagName="...")`, énumérations par nom, booléens `True/False`.

Le fichier complet `Data/DT_Glyphs.csv` contient **36 glyphes, dont 6 secrets** (jeu de départ du vertical slice), pour un coût total de **463 points** (343 hors secrets). L'arbre final visé compte environ 700 points de glyphes pour 496 points gagnés : au vertical slice, un joueur au niveau 80 peut donc tout apprendre ; la contrainte de spécialisation arrivera avec l'arbre complet (à équilibrer).

---

## 9. Boss

Règles communes : PV × (1 + 0,6 × (joueurs − 1)) en coop, échelle figée au début de la rencontre ; posture brisée → étourdi, riposte critique ×3 ; transitions de phase avec invulnérabilité courte et rugissement ; chaque attaque a un télégraphe visuel **et** sonore. Les « fenêtres d'esquive » sont données en secondes depuis le début de l'attaque.

### 9.1 Boss 1 — Le Colosse d'Ossivaire (Gardien terrestre, milieu d'early game)
Golem-chimère antique : corps de pierre runique, tête de lion d'airain, queue de serpent de basalte. Garde la Cour des Rois-Pierre, passage obligé vers l'Acte II.

| Stat | Valeur |
|---|---|
| Niveau conseillé | 18-22 |
| PV (solo) | 12 000 |
| Posture | 600, régénération 25/s après 3 s ; étourdi 4 s |
| Résistances | physique 10 %, feu 40 %, foudre −20 % (faiblesse), corruption 0 % |
| Point faible | noyau runique dans le dos (×1,5), exposé 3 s après le Séisme |
| Phases | **P1 100-50 %** ; **P2 50-0 %** (runes embrasées : télégraphes ×0,9, nouvelles attaques) ; transition : invulnérable 4 s |

| Attaque | Phase | Télégraphe | Fenêtre d'esquive | Dégâts / poise | Contre-jeu |
|---|---|---|---|---|---|
| Poing-marteau | 1-2 | 1,0 s, bras levé, grondement | 0,90-1,15 | 180 / 50 | rouler sur le côté du bras ; 1,5 s de recovery à punir |
| Balayage du bras | 1-2 | 0,8 s, épaule qui recule | 0,70-0,95 | 140 / 40 | rouler vers le Colosse (sous le bras) |
| Morsure du lion | 1-2 | 0,6 s, gueule qui s'ouvre | 0,55-0,75 | 120 / 30 | **parable** : 35 de posture et riposte |
| Fouet serpentin | 1-2 | 0,7 s, queue raidie (joueur derrière) | 0,60-0,85 | 160 / 60 | ne pas rester dans le dos plus de 3 s |
| Charge de bélier | 1-2 | 1,4 s, racle le sol | 1,30-1,60 (latéral) | 220 / 90 | s'il percute un obélisque ou un mur : étourdi 3 s |
| Séisme | 1-2 | 1,8 s, saut | 1,75-2,00 (onde rayon 800) | 200 / 80 | rouler à l'impact ; noyau exposé 3 s |
| Ondes runiques | 2 | 1,0 s, runes qui s'allument | 3 anneaux espacés de 0,8 s | 90 chacun | rouler à travers chaque anneau (i-frames) |
| Rugissement du lion | 2 | 0,5 s | cône 60° | 0 / 80 | sortir du cône : sinon poise brisée puis Poing-marteau |
| Résonance des obélisques | 2 | 3 s de canalisation | — | soigne 3 % des PV par obélisque intact | détruire les 4 obélisques (500 PV) ou infliger 150 de posture pendant la canalisation |

**Arène** : cour circulaire de 50 m, 4 obélisques (P2 : ils s'allument). Récompenses : 6 000 XP, Cœur runique (forge), point d'Éveil, tag `Event.Boss.Killed.Guardian` (révèle le glyphe secret « Noyau du Gardien »).

### 9.2 Boss 2 — Nyxariel, l'Aile Flétrie (entité corrompue ailée, mid game)
Séraphin corrompu à quatre ailes, lance de lumière noire. Combat rapide à mi-distance : il punit la garde passive et la roulade précoce.

| Stat | Valeur |
|---|---|
| Niveau conseillé | 42-48 |
| PV (solo) | 24 000 |
| Posture | 700, régénération 50/s après 2,5 s ; étourdi 3 s |
| Résistances | physique 0 %, feu −25 % (faiblesse), corruption 60 % |
| Phases | **P1 100-60 %** ; **P2 60-25 %** (ombres jumelles) ; **P3 25-0 %** (ailes brisées : au sol, vitesse ×1,2, plongeons) |

| Attaque | Phase | Télégraphe | Fenêtre d'esquive | Dégâts / poise | Contre-jeu |
|---|---|---|---|---|---|
| Estoc fulgurant | 1-3 | 0,5 s, lance qui recule + éclat | 0,45-0,60 (latéral) | 160 / 50 | **parable** ; dash de 1 800 cm en 0,25 s |
| Feinte | 1-3 | 1,0 s lance levée + délai aléatoire 0,3-0,9 s | selon délai | 160 / 50 | ne pas rouler sur la levée : attendre le flash de la pointe |
| Ciseaux d'ailes | 1-3 | 0,4 s | 2 coups à 0,40 et 0,75 s | 120 ×2 / 30 | rouler deux fois ou bloquer |
| Pluie de plumes | 1-2 | 0,9 s en vol stationnaire | éventail de 24 plumes | 25 + 5 Corruption | se mettre sous lui ou derrière une cloche |
| Lance jetée | 1-3 | 0,7 s | projectile 4 000 cm/s | 200 / 60 | parade parfaite = renvoi (300 dégâts au boss) |
| Piqué en spirale | 1-2 | 1,2 s, ombre au sol | impact rayon 400 | 200 / 70 | rouler au flash de l'ombre |
| Ombres jumelles | 2 | 1,5 s | 2 clones (un coup les dissipe) qui répètent ses attaques avec 0,3 s de retard, 30 % des dégâts | — | dissiper les clones ou décaler sa roulade |
| Halo noir | 2-3 | 1,5 s, ailes repliées | sphère rayon 600 | 250 + 20 Corruption | s'éloigner ou rouler à 1,5 s |
| Chute de l'étoile morte | 3 | 0,8 s avant chaque plongeon | 3 plongeons à 0,9 s d'intervalle | 220 / 80 chacun | après le 3e : **vulnérable 4 s** |

**Arène** : Clocher effondré de Vel-Morrow, plates-formes sur trois niveaux ; 3 cloches : frapper une cloche quand il est en vol l'étourdit 2 s (une fois par cloche). Récompenses : 15 000 XP, Plumes du Séraphe, point d'Éveil, tag `Event.Boss.Killed.Seraph` (débloque la Forge draconique, révèle « Plumes du Séraphe »).

### 9.3 Boss final — Vaelthyrax, l'Ancêtre-Braise
Dragon ancestral titanesque, maître de la Caldera de Cendrehaute. Combat en trois phases sur une arène qui se détruit, avec des attaques aériennes punitives et des souffles qui exigent de lire le terrain.

| Stat | Valeur |
|---|---|
| Niveau conseillé | 75-80 |
| PV (solo) | 90 000 (× (1 + 0,6 × (joueurs − 1))) |
| Posture | 1 500, régénération 40/s après 4 s ; étourdi 4 s, riposte critique ×3 |
| Points faibles (selon l'os touché) | tête ×1,3 ; ventre ×1,15 ; ailes ×1,0 ; queue ×0,8 |
| Vitesses | marche 600 cm/s, vol 2 400 cm/s, piqué 4 500 cm/s |
| Phases | **P1 100-70 %** (au sol, arène intacte) ; **P2 70-35 %** (envols fréquents, lave) ; **P3 35-0 %** (enragé : animations ×1,15, recharges ×0,8, télégraphes ×0,9 ; île centrale, foudre) |
| Transition | invulnérable 5 s (rugissement), puis changement d'arène ; un coup qui saute deux seuils déclenche P2 puis P3 à la suite |

**Patterns** (identiques à `Data/DT_DragonAttacks.csv`) :

| Attaque | Phases | Portée (cm) | Angle | Télégraphe | Fenêtre d'esquive | Dégâts / poise | Parable | Recharge | Contre-jeu |
|---|---|---|---|---|---|---|---|---|---|
| Morsure | 1-3 | 0-900 | ±45° | 0,9 s, tête qui recule | 0,80-1,05 | 220 / 60 | **Oui** | 3 s | seule attaque parable : 35 de posture |
| Griffe (double, 2e retardée) | 1-3 | 0-1 100 | ±90° | 1,1 s, patte levée | 1,00-1,25 puis 1,70-1,95 | 260 ×2 / 70 | Non | 4 s | ne pas rouler deux fois de suite trop tôt : le 2e coup est retardé |
| Fouet caudal | 1-3 | 0-1 400 | arrière > 110° | 0,7 s, queue raidie | 0,60-0,85 | 300 / 90 | Non | 5 s | le dragon ne pivote pas : s'éloigner du dos |
| Rafale d'ailes | 1-3 | 0-1 600 | 360° | 1,2 s, ailes déployées | 1,10-1,40 | 80 / 40 + recul 1 200 cm | Non | 10 s | poids ×3 si ≥ 2 joueurs collés : punit l'encerclement |
| Souffle frontal | 1-3 | 1 000-3 500 | ±30° | 1,6 s, gorge incandescente | sortir du cône (20°) dès 1,4 | 35 feu/tick, 8 ticks/s + brûlure 15/s 4 s | Non | 12 s | courir latéralement ; garde : 10 endurance/tick |
| Souffle balayant | 2-3 | 600-3 000 | ±60° | 1,8 s | rouler à travers au passage (120° en 2,0 s) | 40 feu/tick | Non | 16 s | se cacher derrière un pilier |
| Piqué | 2-3 | 1 500-6 000 | tout | envol 2,0 s + ombre 1,5 s | flash 0,35 s avant l'impact | 400 / 120, rayon 600 | Non | 18 s | **vulnérable 2,5 s après l'impact (dégâts reçus ×1,5)** |
| Souffle en vol rasant | 2-3 | 2 000-8 000 | tout | 2,0 s | quitter la ligne | 30 feu/tick ; zones de feu 8 s à 20/s | Non | 20 s | les zones réduisent l'espace : se repositionner tôt |
| Pluie de braises | 3 | tout | tout | 2,5 s (rugissement) ; cercles 1,2 s avant chaque impact | 12 impacts en 6 s, rayon 300 | 180 / 40 | Non | 25 s | lire les cercles, ne pas rouler au hasard |
| Saisie | 3 | 0-800 | ±30° | 1,0 s (rapide) | 0,85-1,10 (esquive **tardive**) | 350 + brûlure / 150 | Non, imblocable | 14 s | piège à roulade précoce |
| Embrasement total (Cataclysm) | 3 | tout | tout | 4,0 s de charge en vol | se cacher derrière un obstacle (ligne de vue) | 900 / 200 | Non, imblocable | entrée en P3 + 20 s, puis toutes les 75 s | atteindre un débris de pilier ou l'alcôve centrale |

**Sélection des attaques (IA)** : score = poids de base × facteur de distance × facteur d'angle × poids de phase × pénalité de répétition (dernière attaque ×0,25, avant-dernière ×0,6) ; attaques en recharge exclues ; tirage aléatoire pondéré. Le joueur ne peut donc pas prévoir l'ordre, mais chaque attaque reste lisible.

**Arène évolutive (Caldera de Cendrehaute)** :
| Phase | État | Danger |
|---|---|---|
| P1 | plateau intact, 6 piliers de basalte, 3 ponts | aucun |
| P2 | 3 piliers détruits, coulées de lave dans les fissures | lave 25 dégâts/s (sur la durée) |
| P3 | effondrement du pourtour : île centrale de 40 m de rayon, tempête de cendres | éclair toutes les 3,0 s sur un joueur (marqueur 1,5 s, 150 dégâts, rayon 250) |

**Récompenses** : 60 000 XP, EmberHeart ×3, Couronne de l'Ancêtre (cosmétique), point d'Éveil, tag `Event.Boss.Killed.Dragon` (fin de campagne, NG+, glyphe secret « Cœur-Braise lié »).

**Behavior Tree** : structure et code dans `02_Architecture.md` §4 et `Source/Emberfall/Public/EMBDragonAIController.h`.

---

## 10. Coop
- 1 à 4 joueurs, **serveur autoritaire** (listen server par défaut, serveur dédié possible : voir questions ouvertes).
- Progression **par personnage** (niveau, points, glyphes) ; le monde (bases, boss vaincus) appartient à l'hôte.
- Boss : PV ×(1 + 0,6 × (joueurs − 1)), échelle figée au début du combat ; récompenses pour chaque joueur présent.
- Pas de dégâts entre joueurs par défaut (option serveur).
- Réanimation d'un allié tombé : prévue (non codée), 5 s d'interaction, 30 % des PV.

---

## 11. Questions ouvertes
1. Durée de campagne visée (60-80 h ici) et taille du monde (hypothèse 8 × 8 km dans `03_Scalabilite.md`).
2. Serveurs dédiés proposés aux joueurs ? (coût d'hébergement récurrent à chiffrer — aucun engagement pris).
3. Équilibrage glyphes : 463 points pour les 36 glyphes du jeu de départ contre 496 gagnés ; arbre final visé ≈ 700 points pour forcer la spécialisation. À confirmer en playtest.
4. Mort du joueur : sac de ressources récupérable (proposé) ou perte d'XP façon souls ?
5. Version du moteur figée : 5.4 ou plus récente (impacte Nanite végétation, Iris).
