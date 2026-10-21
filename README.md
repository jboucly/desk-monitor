# Desk Monitor

Écran tactile de bureau 480×480 piloté par [ESPHome](https://esphome.io) et [LVGL](https://lvgl.io), branché sur Home Assistant. Il affiche l'état du serveur Proxmox, commande les lumières pièce par pièce et donne la météo.

L'interface tourne à l'identique sur l'écran physique (Guition ESP32-S3-4848S040) et dans un simulateur sur ordinateur (SDL), pratique pour tester sans flasher.

## Fonctionnalités

| Page | Contenu |
|---|---|
| **Tableau de bord** | Proxmox VE : statut, uptime, jauges CPU / RAM / disque, stockage des VM. Les jauges passent à l'orange au-delà de 75 % et au rouge au-delà de 90 %. |
| **Bureau** | Lumière principale et lumière du bureau, sur de grandes cartes. |
| **Salon** | Mur, lampe d'appoint et LED du plan de travail. |
| **Météo** | Conditions actuelles, humidité, vent, pression et prévisions sur 5 jours (Met.no). |

Chaque carte lumière affiche la couleur réelle de l'ampoule (pastille, interrupteur et curseur), son état et sa luminosité.

### Gestes

| Geste | Effet |
|---|---|
| Glisser à gauche / droite | Page suivante / précédente (la dernière page revient à la première). |
| Tap sur une carte lumière | Allume / éteint. |
| Glisser le curseur d'une carte | Règle la luminosité. |
| **Appui long sur une carte lumière** | Ouvre le popup couleur : roue des teintes et saturation (onglet « Couleur »), température de blanc en Kelvin (onglet « Blanc »), luminosité, et marche / arrêt au centre. L'appui long est sans effet sur une lumière qui ne gère ni la couleur ni la température de blanc. |
| Appui long sur la barre du haut | Passe du thème sombre au thème clair, et inversement. |

Le thème est aussi exposé dans Home Assistant (select « Current Theme »), ce qui permet de l'automatiser, par exemple « Light » au lever du soleil et « Dark » au coucher.

## Prérequis

- ESPHome **2026.7.0 ou plus récent**. La version 2026.9.1 est testée.
- La librairie [esphome-modular-lvgl-buttons](https://github.com/agillis/esphome-modular-lvgl-buttons), clonée à la racine du dépôt :
  ```sh
  git clone https://github.com/agillis/esphome-modular-lvgl-buttons.git
  ```
- Pour le simulateur : SDL2 (`brew install sdl2` sur macOS).
- Un fichier `secrets.yaml` à la racine (non versionné) :
  ```yaml
  wifi_ssid: "..."
  wifi_password: "..."
  ap_password: "..."
  ```

## Utilisation

```sh
# Simulateur (fenêtre 480×480, la souris remplace le doigt)
esphome run esphome-desk-monitor-sdl.yaml

# Écran physique : compile puis flashe (USB la première fois, OTA ensuite)
esphome run esphome-desk-monitor.yaml

# Vérifier la configuration sans compiler
esphome config esphome-desk-monitor.yaml
```

Une fois l'appareil ajouté dans Home Assistant, autorise-le à **effectuer des actions Home Assistant** (Paramètres → Appareils et services → ESPHome → l'appareil → Configurer). Sans cette autorisation, l'écran ne peut ni commander les lumières ni récupérer les prévisions météo.

## Personnalisation

| Pour… | Modifier |
|---|---|
| Changer une entité HA (lumière, capteur, météo) | les substitutions en haut de [`desk-monitor/ui.yaml`](desk-monitor/ui.yaml) |
| Changer l'ordre des pages | l'ordre des `packages:` dans [`desk-monitor/ui.yaml`](desk-monitor/ui.yaml) |
| Ajouter une lumière | un bloc `widgets/light_card.yaml` dans la page voulue (voir [`pages/salon.yaml`](desk-monitor/pages/salon.yaml)) |
| Changer les couleurs des thèmes | la palette de [`desk-monitor/dm_theme.h`](desk-monitor/dm_theme.h) |
| Utiliser une nouvelle icône | ajouter son `$mdi_xxx` dans la police de la bonne taille, dans [`desk-monitor/core.yaml`](desk-monitor/core.yaml) (sinon la compilation échoue) |
| Rétroéclairage, mise en veille, rotation | les substitutions de [`esphome-desk-monitor.yaml`](esphome-desk-monitor.yaml) |

La liste des entités disponibles se trouve dans `ha-entities.json`, un export du registre d'entités de Home Assistant.

## Structure

```
esphome-desk-monitor.yaml       écran physique : Wi-Fi, OTA, rétroéclairage, tactile
esphome-desk-monitor-sdl.yaml   simulateur sur ordinateur
desk-monitor/                   interface commune aux deux
  ui.yaml                       entités Home Assistant + ordre des pages
  core.yaml                     thème, styles, polices d'icônes, popups, scripts communs
  dm_theme.h                    palette sombre / clair et helpers C++
  pages/                        une page par fichier
  widgets/                      briques réutilisables (en-tête, carte lumière, popup couleur, jauge…)
esphome-modular-lvgl-buttons/   librairie externe (matériel, polices, thème de base)
```

Les deux fichiers d'appareil ne contiennent que ce qui est propre au matériel. Toute modification dans `desk-monitor/` s'applique donc à la fois à l'écran et au simulateur.

## Crédits

- [esphome-modular-lvgl-buttons](https://github.com/agillis/esphome-modular-lvgl-buttons) : support du matériel, thème de base et anneaux de couleur du popup lumière.
- Icônes [Material Design Icons](https://pictogrammers.com/library/mdi/), police [Nunito](https://fonts.google.com/specimen/Nunito).
