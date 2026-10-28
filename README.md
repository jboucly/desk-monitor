# Desk Monitor

Écran tactile de bureau 480×480 (Guition ESP32-S3-4848S040) qui sert d'**extension de l'ordinateur**. Il fonctionne seul en Wi-Fi : Home Assistant, météo, horloge. Une application compagnon sur l'ordinateur s'y connecte par le réseau local pour afficher les métriques du PC et exécuter des raccourcis façon Stream Deck (la musique arrive en phase 3).

Les pages se configurent **à la volée** depuis un éditeur web servi par l'écran lui-même (`http://desk-monitor.local`), sans recompiler ni reflasher.

> La v1 (configuration ESPHome) est archivée dans [`legacy/esphome/`](legacy/esphome/) et reste flashable.

## État d'avancement

| Phase | Contenu | État |
| --- | --- | --- |
| 1 | Firmware natif (ESP-IDF + LVGL 9), design « glass », Wi-Fi, Home Assistant, météo Open-Meteo, layout JSON, éditeur web, OTA | ✅ |
| 2 | Application compagnon (Rust/Tauri, macOS et Windows) reliée par le réseau local : métriques PC, raccourcis clavier, lancement d'applications et d'URL | ✅ (testée sur macOS) |
| 3 | Contrôle de la musique et pochette de l'album en cours | à venir |

## Widgets

| Widget | Rôle |
| --- | --- |
| Horloge | Heure et date en français, directement sur le fond d'écran |
| Météo | Conditions actuelles ou prévisions jusqu'à 7 jours (Open-Meteo, sans clé d'API) |
| Lumière HA | Tap pour allumer/éteindre ; **appui long** pour ouvrir la popup avec la roue chromatique, la température de blanc et la luminosité |
| Interrupteur HA | Prise, ventilateur, scène, script, volet… (le service adapté au domaine est appelé) |
| Capteur HA | Valeur et unité |
| Jauge HA | Arc à 270°, seuils orange/rouge configurables |
| Raccourci | Action Home Assistant, ou action sur le PC (raccourci clavier, application, URL) |
| Métriques PC | CPU, RAM, GPU (avec températures) et débit réseau de l'ordinateur appairé |
| Jauge PC | Une mesure du PC : CPU, RAM, GPU, mémoire GPU, disque ou température |

Le widget musique s'affiche « Bientôt » jusqu'à la phase 3.

## Démarrage rapide

### Prérequis

- **ESP-IDF 5.5**. Celui qu'installe ESPHome est réutilisé : `. firmware/tools/idf_env.sh`.
- **SDL2** et **libcurl** pour le simulateur : `brew install sdl2 curl`.
- **Node.js 20+** pour l'éditeur web.

### Simulateur (sans matériel)

```sh
cd web && npm install && npm run build && cd ..          # éditeur web (servi par le simulateur)
cmake -S firmware/sim -B firmware/sim/build -G Ninja    # cmake/ninja : ceux d'ESP-IDF après idf_env.sh
cmake --build firmware/sim/build
firmware/sim/build/desk_monitor_sim                      # fenêtre 480×480 + éditeur sur http://localhost:8080
```

Dans le simulateur, Home Assistant et le PC sont simulés : les lumières réagissent comme dans HA. La météo, elle, est bien réelle.

### Écran physique

```sh
cd web && npm run build && cd ..        # l'éditeur est embarqué dans le firmware
. firmware/tools/idf_env.sh
cd firmware && idf.py build flash monitor
```

Au premier démarrage, l'écran ouvre le Wi-Fi **DeskMonitor-setup**. Connecte-toi à ce réseau, ouvre `http://192.168.4.1` puis, dans *Réglages* :

1. **Wi-Fi** : renseigne le réseau de la maison.
2. **Home Assistant** : saisis l'adresse (`http://homeassistant.local:8123`) et un jeton d'accès longue durée (Profil → Sécurité → Jetons d'accès longue durée).
3. **Météo** : cherche ta ville.

Une fois l'écran connecté, l'éditeur est accessible depuis n'importe quel navigateur du réseau : **http://desk-monitor.local**. Les mises à jour suivantes se font depuis l'onglet *Système*, en envoyant `firmware/build/desk_monitor.bin`.

### Application compagnon (ordinateur)

```sh
cd companion && npm install
npm run dev                                              # lance l'app en mode développement
npm run build                                            # .app / .dmg (macOS) ou .msi (Windows) dans src-tauri/target/release/bundle/
```

L'app vit dans la barre des menus. Au premier lancement, elle trouve l'écran (`desk-monitor.local`) et propose l'appairage : l'écran affiche un code à 6 chiffres à saisir dans l'app. Pour tester avec le simulateur, mets l'adresse `localhost:8081`. Sur macOS, les raccourcis clavier demandent l'autorisation *Accessibilité* (bouton dans l'app). Le PC appairé se gère dans l'éditeur, *Réglages → Ordinateur*.

### Développer l'éditeur web

```sh
cd web && npm run dev                                    # API proxifiée vers le simulateur (localhost:8080)
DM_DEVICE=http://desk-monitor.local npm run dev          # … ou vers l'écran réel
```

## Structure

```
firmware/
  core/        C++ portable : bus de données, layout, thème glass, widgets, session HA, météo, API REST
  main/        couche ESP32 : écran ST7701S + tactile GT911, LittleFS, Wi-Fi, WebSocket HA, HTTP, OTA
  sim/         simulateur SDL2 (même code core/, HA et PC simulés, captures PNG scriptables)
  data/        layout par défaut
  tools/       génération des polices et icônes, environnement ESP-IDF
web/           éditeur (Vite + Preact + TypeScript), compilé en une page gzippée embarquée dans le firmware
companion/     application compagnon (Rust + Tauri) : métriques, raccourcis
protocol/      contrats partagés : schéma du layout, liste d'icônes, API, lien PC
docs/hardware/ documentation constructeur de l'écran (non versionnée)
legacy/esphome/ version 1 (ESPHome)
```

## Crédits

- [LVGL](https://lvgl.io), [ESP-IDF](https://github.com/espressif/esp-idf) et ses composants (`esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_lvgl_port`).
- Séquence d'initialisation de l'écran reprise du modèle `GUITION-4848S040` d'[ESPHome](https://esphome.io).
- Police [Inter](https://rsms.me/inter/), icônes [Material Design Icons](https://pictogrammers.com/library/mdi/), météo [Open-Meteo](https://open-meteo.com).
