# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESPHome configuration for a "Desk Monitor": a 480×480 touch screen (Guition ESP32-S3-4848S040) running an LVGL UI that shows Home Assistant data (Proxmox server gauges, lights per room, weather). The UI text is in French.

## Commands

```sh
esphome config  esphome-desk-monitor-sdl.yaml   # validate the merged config (fast)
esphome compile esphome-desk-monitor-sdl.yaml   # build the desktop simulator (~15 s, SDL2 installed)
esphome run     esphome-desk-monitor-sdl.yaml   # open the 480×480 simulator window (mouse = touch)
esphome run     esphome-desk-monitor.yaml       # build + flash the real ESP32 (OTA or USB)
```

There are no tests or linters: validate every change with `esphome config` on both device files, and `esphome compile` the SDL target (C++ lambdas only fail at compile time). The simulator connects to Home Assistant through the `api:`; without HA every light is "unavailable", so the light popups don't open.

`secrets.yaml` (Wi-Fi, gitignored) is required for the ESP32 target. `ha-entities.json` is an export of the HA entity registry, used to pick entity ids. It has no area and no attribute info, so light capabilities are detected at runtime.

## Architecture

**Two device files, one shared UI.** `esphome-desk-monitor.yaml` (hardware: Wi-Fi, OTA, backlight, touch calibration, RSSI polling) and `esphome-desk-monitor-sdl.yaml` (simulator) only hold device-specific bits. Both include `desk-monitor/ui.yaml`. Every UI change goes in `desk-monitor/` and applies to both.

**`esphome-modular-lvgl-buttons/`** is a vendored, untracked clone of the upstream library (github.com/agillis/esphome-modular-lvgl-buttons). Treat it as read-only. The device files pull from it: hardware definitions, `common/theme/index.yaml` (Nunito fonts, `$mdi_xxx` glyph substitutions, the `current_theme` select), sensors, the time source and the loading page. Assets such as the color-ring SVGs are referenced by path from `desk-monitor/core.yaml`. The library has its own CLAUDE.md, which describes its conventions.

**`desk-monitor/` layout:**
- `ui.yaml`: HA entity ids (substitutions) and page order. Pages appear in the order of their `packages:` entries.
- `core.yaml`: shared styles (`dm_page`, `dm_card`, `dm_chip`, `dm_text`, `dm_text2`…), the theme switch, the popup backdrop and close/timeout scripts, the MDI icon fonts, and the ring images.
- `dm_theme.h`: C++ included via `esphome: includes:`. The `dm::` namespace provides the dark/light palette (`dm::bg()`, `dm::accent()`…), slider styling helpers (`dm::fill_slider`, `dm::gradient_slider`), color conversions, and weather/uptime helpers.
- `pages/*.yaml`: one page each; they compose widgets.
- `widgets/*.yaml`: reusable blocks included with `!include {file, vars}`. Every id is prefixed with `${uid}` (or `${page_id}`) so that a widget can be instantiated several times.

**Hook scripts extended with `!extend`.** `core.yaml` declares empty scripts that widgets extend:
- `time_update`: runs every minute.
- `dm_theme_refresh`: recolors dynamic elements when the theme changes.
- `dm_wifi_refresh`: updates the header Wi-Fi icons.
- `dm_popup_close`: each popup hides itself.

**Theming.** Shared styles are rewritten by the `dm_apply_theme` script from `dm::` colors. Anything colored at runtime (light cards, gauges…) is painted in a widget-level `${uid}_render` lambda that reads `dm::` colors. The render lambda also runs on `dm_theme_refresh`. Colors are not hardcoded in YAML except for the initial dark values of the styles.

**Light card + popup.** `widgets/light_card.yaml` subscribes to HA attributes with `homeassistant` text_sensors and stores them in `${uid}_*` globals. It detects capabilities from `supported_color_modes`, renders through `${uid}_render`, and pulls in `widgets/light_popup.yaml` via `packages:`. The popup lives in `lvgl: top_layer` and opens on a long press. It offers a hue ring or a Kelvin ring, plus brightness and saturation sliders. It sends `light.turn_on` with `hs_color`, `color_temp_kelvin` or `brightness_pct` through `homeassistant.action`.

## Gotchas

- **Icon glyphs must be declared.** Any `$mdi_xxx` used in the UI must be listed in the matching font (`mdi_24`, `mdi_32`, `mdi_icons_40`, `mdi_72`) in `core.yaml`, or the build fails.
- **`top_layer` order is z-order.** `dm_popup_scrim` (in `core.yaml`) must stay before the popups that widgets add.
- **Library binding labels.** `core.yaml` declares hidden labels (`esphome_version_label`, `ip_address_label`…) that the library sensors write into. Don't remove them.
- **Swipe navigation.** Pages merge `widgets/swipe.yaml` via `<<: !include`. Sliders inside pages need `gesture_bubble: false` so that dragging them doesn't change the page.
- **Lambdas that touch LVGL objects** use the LVGL 9 API (e.g. `lv_obj_remove_flag`, `lv_color_luminance`).
- **Unresolved-tag warnings.** VS Code YAML diagnostics such as "Unresolved tag: !include/!extend/!lambda" are false positives.
