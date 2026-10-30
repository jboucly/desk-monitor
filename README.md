# Desk Monitor

**English** · [Français](README.fr.md)

A 480×480 desk touch screen (Guition ESP32-S3-4848S040) that acts as an **extension of your computer**. It works on its own over Wi-Fi: Home Assistant, weather, clock. A companion app on the computer connects to it over the local network: it sends PC metrics and the music currently playing, and runs Stream Deck-style shortcuts.

Pages are configured **on the fly** from a web editor served by the screen itself (`http://desk-monitor.local`), with no rebuild and no reflash.

> The on-screen UI and the web editor are in French.

| | | |
| --- | --- | --- |
| ![Home](docs/images/home.png) | ![Computer](docs/images/computer.png) | ![Music](docs/images/music.png) |
| ![Lights](docs/images/lights.png) | ![Weather](docs/images/weather.png) | ![Shortcuts](docs/images/shortcuts.png) |

*Simulator screenshots, default layout.*

## Status

| Phase | Content | Status |
| --- | --- | --- |
| 1 | Native firmware (ESP-IDF + LVGL 9), dark theme, Wi-Fi, Home Assistant, Open-Meteo weather, JSON layout, web editor | ✅ |
| 2 | Companion app (Rust/Tauri) linked over the local network: PC metrics, keyboard shortcuts, launching apps and URLs | ✅ macOS (Windows written, never compiled) |
| 3 | Now playing (every Mac app, browsers included) with cover art and controls; falls back to a Home Assistant media player | ✅ macOS (Windows still to write) |

## Hardware

- **Guition ESP32-S3-4848S040**: 4" 480×480 IPS panel (ST7701S, RGB interface), GT911 capacitive touch, ESP32-S3 with 16 MB flash and 8 MB PSRAM.
- A USB-C cable. Nothing to solder.

## Widgets

| Widget | What it does |
| --- | --- |
| Clock | Time and date, on the background or in a card |
| Weather | Current conditions or a forecast of up to 7 days (Open-Meteo, no API key) |
| HA light | Tap to toggle; **long press** opens a popup with a color wheel, white temperature and brightness |
| HA switch | Plug, fan, scene, script, cover… (the service matching the domain is called) |
| HA sensor | Value and unit |
| HA gauge | 270° arc, configurable orange/red thresholds |
| Shortcut | Home Assistant action, or an action on the PC (keyboard shortcut, app, URL) |
| PC metrics | CPU, RAM, GPU (with temperatures) and network throughput of the paired computer |
| PC gauge | One PC measure: CPU, RAM, GPU, GPU memory, disk or temperature |
| Music | What the computer plays (Music, Spotify, Chrome, Safari…) with cover art, progress and controls; otherwise the chosen Home Assistant player |

The PC widgets and the computer's music need the companion app.

### Themes

The style is picked in the editor (*Thème → Style*):

- **Flat dark** (default): the smoothest.
- **Frozen glass**: cards tinted like glass over a colored background, almost as fast.
- **Translucent glass**: the prettiest, but about 2.5 times slower to draw (choppy swipes).

## Quick start

### Requirements

- **ESP-IDF 5.5.5** in `~/esp/esp-idf` (or `$DM_IDF_PATH`): `git clone -b v5.5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf && ~/esp/esp-idf/install.sh esp32s3`, then `. firmware/tools/idf_env.sh` in each terminal.
- **SDL2** and **libcurl** for the simulator: `brew install sdl2 curl`.
- **Node.js 20+** for the web editor and the companion app.
- **Rust** ([rustup](https://rustup.rs)) and the Xcode command line tools for the companion app.

### Simulator (no hardware)

```sh
cd web && npm install && npm run build && cd ..          # web editor (served by the simulator)
cmake -S firmware/sim -B firmware/sim/build -G Ninja    # cmake/ninja: ESP-IDF's after idf_env.sh
cmake --build firmware/sim/build
firmware/sim/build/desk_monitor_sim                      # 480×480 window + editor on http://localhost:8080
```

Home Assistant is simulated: the lights react as they would in HA. The weather is real. The simulator waits for the real companion app on `localhost:8081`, or fakes a PC (metrics and music) with `--mock-pc`.

### Real screen

```sh
cd web && npm run build && cd ..        # the editor is embedded in the firmware
. firmware/tools/idf_env.sh
cd firmware && idf.py build flash monitor
```

On first boot, the screen opens the **DeskMonitor-setup** Wi-Fi network. Join it, open `http://192.168.4.1`, then in *Réglages* (settings):

1. **Wi-Fi**: enter your home network.
2. **Home Assistant**: enter the address (`http://homeassistant.local:8123`) and a long-lived access token (Profile → Security → Long-lived access tokens).
3. **Météo** (weather): search for your city.

Once the screen is connected, the editor is reachable from any browser on the network: **http://desk-monitor.local**. Later updates go over the air from the *Système* tab, by sending `firmware/build/desk_monitor.bin`. From a terminal: `curl --data-binary @firmware/build/desk_monitor.bin http://desk-monitor.local/api/ota`.

### Companion app (computer)

```sh
cd companion && npm install
npm run dev                                              # run the app in development mode
npm run build                                            # .app / .dmg (macOS) or .msi (Windows) in src-tauri/target/release/bundle/
```

To install it, drag `Desk Monitor.app` (in `src-tauri/target/release/bundle/macos/`) into *Applications*, then tick "Lancer au démarrage" (launch at login) in its window.

The app lives in the menu bar. On first launch, it finds the screen (`desk-monitor.local`) and offers to pair: the screen shows a 6-digit code to type in the app. To try it with the simulator, set the address to `localhost:8081`. The paired PC is managed in the editor, *Réglages → Ordinateur*.

On macOS, the app asks for *Local Network* access, and keyboard shortcuts need the *Accessibility* permission (button in the app). The music currently playing is read with [mediaremote-adapter](https://github.com/ungive/mediaremote-adapter), like boring.notch does: no extra permission.

### Working on the web editor

```sh
cd web && npm run dev                                    # API proxied to the simulator (localhost:8080)
DM_DEVICE=http://desk-monitor.local npm run dev          # … or to the real screen
```

## Layout

```
firmware/
  core/        portable C++: data bus, layout, themes, widgets, HA session, PC link, weather, covers, REST API
  main/        ESP32 layer: ST7701S display + GT911 touch, LittleFS, Wi-Fi, HA and PC WebSockets, HTTP, OTA
  bootloader_components/  backlight off from boot (no white flash)
  sim/         SDL2 simulator (same core/ code, simulated HA and PC, scriptable PNG screenshots)
  data/        default layout
  tools/       font and icon generation, ESP-IDF environment
web/           editor (Vite + Preact + TypeScript), built into one gzipped page embedded in the firmware
companion/     companion app (Rust + Tauri): metrics, shortcuts, now playing
  vendor/      mediaremote-adapter (macOS "Now Playing")
protocol/      shared contracts: layout schema, icon list, API, PC link
docs/images/   screenshots
```

The architecture, the threading rules and the hardware gotchas are described in [CLAUDE.md](CLAUDE.md). The protocols (bus keys, REST API, PC link) are in [protocol/README.md](protocol/README.md).

## Credits

- [LVGL](https://lvgl.io), [ESP-IDF](https://github.com/espressif/esp-idf) and its components (`esp_lcd_st7701`, `esp_lcd_touch_gt911`, `esp_lvgl_port`).
- Display init sequence taken from ESPHome's `GUITION-4848S040` model ([ESPHome](https://esphome.io)).
- [Tauri](https://tauri.app) for the companion app, [mediaremote-adapter](https://github.com/ungive/mediaremote-adapter) (BSD-3) for now playing on macOS, TJpgDec (bundled with LVGL) for Home Assistant covers.
- [Inter](https://rsms.me/inter/) font, [Material Design Icons](https://pictogrammers.com/library/mdi/), weather by [Open-Meteo](https://open-meteo.com).
- Third-party licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Contributing

Issues and pull requests are welcome: see [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) © Julien Boucly
