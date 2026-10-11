# Phibres — by Phragma

Effet audio VST3 pour macOS et Windows : étirement du temps par grains, sur une capture du son qui passe.

**v0.1.0 — première version à écouter.** Tout ce qui est décrit ici a été vérifié par des mesures (voir « Tests »), mais rien n'a encore été réglé à l'oreille. Le moteur spectral, la courbe de vitesse et la modulation (LFO, grille de pas) viendront ensuite.

## Principe

Phibres se pose en insert. Quand STRETCH s'allume, il enregistre l'entrée sur une bande. Une tête de lecture parcourt la boucle capturée à la vitesse voulue, et lance un grain à intervalle régulier. Chaque grain relit la bande à sa propre hauteur : la vitesse de la tête et la hauteur du son sont indépendantes.

- Pas de latence. Éteint, le son passe intact, à l'échantillon près.
- La tête ne lit jamais le futur : tant que la capture est en cours, un grain qui irait plus loin que l'enregistrement est reculé. À l'allumage le son continue donc sans trou, puis ralentit.
- À vitesse réelle et sans transposition, le premier passage est transparent ; ensuite la capture tourne en boucle.

## Les cinq modules

- **CAPTURE**
  - **STRETCH** : allume l'effet. La capture part de cet instant.
  - **LENGTH** : longueur de la boucle parcourue par la tête. Tourner à droite raccourcit. **SYNC** : 8/1 (8 mesures) à 1/32. **FREE** : 30 s à 20 ms. La bande est limitée à 30 s.
  - **POSITION** : décale la tête dans la boucle. Tête gelée, c'est ce potard qui la promène.
  - **AUTO** + **EVERY** : recapture périodique (16/1 à 1/16), comptée depuis la dernière capture.
  - **GRAB** : recapture immédiate. Une note MIDI envoyée au plugin fait de même, à l'échantillon près.
  - L'afficheur montre la capture, ce qui reste à enregistrer (pointillé) et la tête.
- **STRETCH**
  - **FREE** : facteur d'étirement, de x1 à x1000. **SYNC** : durée sur laquelle la boucle est étalée (1/4 à 64/1) ; `LENGTH 1/4` et `OVER 4/1` étirent un temps sur quatre mesures. Si cette durée est plus courte que la boucle, la tête va plus vite que le temps réel, jusqu'à quatre fois.
  - **FREEZE** : arrête la tête.
  - **PITCH** : hauteur des grains, de -24 à +24 demi-tons. **SEMI** : par demi-tons. **FREE** : au centième.
  - **REVERSE** : la tête recule.
  - L'afficheur donne la durée de la boucle et celle d'un passage une fois étiré.
- **GRAIN**
  - **RATE** : intervalle entre deux départs de grain. **FREE** : 1 s à 1 ms. **SYNC** : 1/1 à 1/128, calé sur la grille de l'hôte quand le transport tourne. **NOTE** : accordé sur une note, de 16 Hz à 1047 Hz (C-1 à C5 dans la notation d'Ableton Live, où le do central est C3).
  - **FLIP** : chaque grain est lu à l'envers.
  - **SIZE** : durée d'un grain, de 25 % à 800 % de l'intervalle. 200 % = recouvrement de moitié.
  - **SHAPE** : fenêtre du grain, de presque rectangulaire (bords de 1 ms) à la cloche de Hann.
  - **TILT** : penche la fenêtre vers une attaque franche (à gauche) ou une fin franche (à droite).
- **SCATTER** : un tirage par grain, que le bouton allume ou éteint d'un coup.
  - **SPRAY** : écart de position, jusqu'à 1 s. **DETUNE** : écart de hauteur, jusqu'à 24 demi-tons (**FINE** continu, **SEMI** par demi-tons, **OCT** par octaves). **FLIP** : probabilité d'inverser le sens du grain. **WIDTH** : écart de panoramique.
- **FILTER** : filtre résonant en fin de chaîne (HP / BP / LP, 12 ou 24 dB par octave, RESO), **MIX** dry / wet, **LIMITER** à 0 dBFS, actif que le filtre soit allumé ou non.

39 paramètres, tous automatisables. Mono et stéréo.

## Ce qu'il faut savoir

- **Le son des grains.** C'est un granulaire brut, sans alignement de phase : plusieurs grains qui se recouvrent lisent le même son à des instants différents, ce qui le colore d'un peigne à la cadence des grains. Sur un son étiré, les partiels se rangent sur une grille espacée de la cadence des grains : avec un intervalle court, on entend une note. À 200 BPM, 1/128 donne 107 Hz. Le mode NOTE sert à choisir cette note.
- **Niveau.** Dès que les grains se superposent, leur somme est divisée par la somme de leurs fenêtres : des grains qui lisent le même son ressortent au niveau de l'entrée, quels que soient leur nombre et leur forme, et la sortie des grains ne dépasse jamais la crête de ce qui a été capturé (à l'interpolation près quand le son est transposé). Sur un son bruité, des grains longs sortent plus bas : environ -4 dB à 400 %, -7 dB à 800 %.
- **Largeur.** Un grain envoyé d'un côté s'efface de l'autre, sans gain du côté où il va : WIDTH ne fait jamais monter un canal.
- **Transposition vers l'aigu.** Pas de filtre anti-repliement : au-dessus de +12 demi-tons sur un son riche en aigus, du repliement s'entend.
- **Allumage avec une transposition vers l'aigu.** Les premiers grains lisent la seconde de son qui précède la capture (elle est gardée en mémoire en permanence).
- **Recapture.** Les grains déjà partis finissent sur l'ancienne capture pendant que les nouveaux partent sur la nouvelle : le passage se fait en fondu, sur la durée d'un grain.
- **Sauvegarde.** La capture est enregistrée avec le projet (la boucle, une demi-seconde avant et deux secondes après, sur 24 bits) : environ 1 Mo pour une boucle d'une mesure, 9 Mo pour 30 s. À la réouverture, STRETCH allumé, la lecture reprend sur le même son, y compris à une autre fréquence d'échantillonnage.
- **Mémoire.** Deux bandes de 37 s par instance : 28 Mo à 48 kHz en stéréo.
- **Bypass de l'hôte.** La capture est abandonnée ; une nouvelle démarre à la réactivation.
- **Tempo.** Si l'hôte n'en fournit pas, le dernier tempo connu est utilisé (120 BPM par défaut).
- **Précision.** STRETCH et GRAB prennent effet au début du bloc audio ; une note MIDI, à l'échantillon près.
- L'interface affiche son numéro de version en haut à droite : c'est le moyen le plus rapide de vérifier ce que le DAW a chargé.

## Tests

- `Tests/EngineTests.cpp` (moteur seul, sans JUCE, à lancer avec les sanitizers) : passage intact éteint ; transparence à vitesse 1 puis boucle exacte ; lecture exacte des grains (position, sens, hauteur) ; durée et hauteur d'un étirement x4 ; transposition ; vitesse, gel, marche arrière et bouclage de la tête ; aucune lecture au-delà de l'enregistrement ; recaptures périodiques et manuelles ; changements de bande sur de longues sessions ; niveaux conformes au calcul ; absence de clic ; largeur stéréo ; dispersion ; calage sur la grille de l'hôte ; capture rechargée ; filtre et limiteur ; réglages au hasard de 8 à 192 kHz.
- `Tests/PluginTests.cpp` (plugin complet et interface) : tempo, modes, triolets et pointées ; cadence des grains ; GRAB, notes MIDI, recapture automatique ; mix, filtre, limiteur ; liste et textes des paramètres ; sauvegarde des réglages et de la capture (24 bits, autre fréquence d'échantillonnage, états abîmés) ; bypass et reset ; interface (contrôles, afficheurs, saisie au clavier, captures d'écran).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPHIBRES_BUILD_TESTS=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Obtenir les .vst3

Chaque envoi sur `main` lance un build GitHub Actions qui compile le plugin pour macOS (universel Apple Silicon + Intel) et Windows (x64), lance les tests du moteur et valide le résultat avec pluginval au niveau 10. Les archives `Phibres-macOS.zip` et `Phibres-Windows.zip` sont en bas de la page du run, section **Artifacts**.

## Installation

Voir `INSTALLATION.txt` (inclus dans chaque archive). En résumé :

- **macOS** : copier `Phibres.vst3` dans `/Library/Audio/Plug-Ins/VST3/`, puis dans Terminal :
  `xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/VST3/Phibres.vst3`
  (le plugin n'est pas notarisé par Apple, macOS le bloque sinon).
- **Windows** : copier le dossier `Phibres.vst3` dans `C:\Program Files\Common Files\VST3\`.

## Licences

Interface : police Chakra Petch (licence OFL, voir `Assets/ChakraPetch-OFL.txt`). Le plugin est construit avec JUCE 8 : vérifier les conditions de la licence JUCE avant toute distribution commerciale.
