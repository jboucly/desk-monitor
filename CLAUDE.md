# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Desk Monitor v2: a 480×480 touch screen (Guition ESP32-S3-4848S040, ST7701S RGB panel, GT911 touch, 16 MB flash, 8 MB PSRAM) that acts as an extension of the PC. It works standalone over Wi-Fi (Home Assistant, Open-Meteo weather, clock). A PC companion (Rust + Tauri, `companion/`) connects over the local network (WebSocket) and adds metrics, shortcuts and the system's now playing. Pages come from a JSON layout edited in a web editor that the screen serves itself. UI text is in French; code and comments are in English.

Phase status: phases 1 (firmware, simulator, web editor), 2 (PC link + companion) and 3 (now playing + covers, HA fallback) are done. Everything companion-side was tested on macOS only: the Windows code is not even compile-checked, and Windows now playing (GlobalSystemMediaTransportControls) is not written yet. The PC link replaced the BLE plan: BLE needed internal RAM the ESP32 does not have. See `protocol/README.md`.

`legacy/esphome/` is the archived v1 (ESPHome config, still flashable, has its own CLAUDE.md). Don't modify it for v2 work.

## Commands

```sh
. firmware/tools/idf_env.sh                       # ESP-IDF 5.5.5 from ~/esp/esp-idf or $DM_IDF_PATH (also puts cmake/ninja on PATH)

# Simulator (validate every UI change here first)
cmake -S firmware/sim -B firmware/sim/build -G Ninja && cmake --build firmware/sim/build
firmware/sim/build/desk_monitor_sim                                   # SDL window + API/editor on :8080
firmware/sim/build/desk_monitor_sim --no-window --port 8099           # no window; GET /sim/screenshot.png
firmware/sim/build/desk_monitor_sim --do page=2 --do long=355,150 --do wait=300 --do shot=/tmp/x.png
                                                                      # scripted headless run (simulated clock)

# Device
cd firmware && idf.py build                       # set-target esp32s3 is needed once after deleting sdkconfig
idf.py flash monitor

# Web editor
cd web && npm run build                           # typecheck + single-file build + dist/index.html.gz
npm run dev                                       # proxies /api to the simulator (DM_DEVICE=… for a real screen)

# Fonts and icon tables (after editing protocol/icons.json)
node firmware/tools/gen_assets.mjs

# PC companion (Rust/Tauri 2; Rust via rustup)
cd companion && npm install && npm run dev        # menu bar app; set the address to localhost:8081 for the simulator
cd companion/src-tauri && cargo test              # key chord parsing + a metrics sample
firmware/sim/build/desk_monitor_sim --mock-pc     # fake PC metrics and now playing without the companion
```

The firmware has no unit tests (the companion has a few `cargo test`s). To verify a change:

- Build the simulator and look at headless screenshots (`--do … shot=`). Read the PNGs.
- Run `idf.py build`, because the ESP32 target catches C++ differences (e.g. `-Werror=stringop-truncation`, designated-initializer order).
- Run `npm run build`.
- Exercise the API with curl against `--no-window`.

The simulator's state (layout.json, settings.json) lives in `firmware/sim/build/data/`. Delete it to get back to the default layout.

## Architecture

**Portable core + two platforms.**

- `firmware/core/` is plain C++17 + LVGL 9.5 + cJSON. It builds as an ESP-IDF component and as a static library for the simulator.
- `firmware/main/` is the ESP32 platform.
- `firmware/sim/` is the desktop platform.

Anything that is not hardware or transport goes in `core/`: HA protocol logic, weather parsing, API routing, settings, UI.

**Data bus (`dm::Bus`)** is the only path from data sources to widgets. Sources call `post(key, cJSON*)` from any thread. `App` runs `bus.dispatch()` in an LVGL timer, so listeners always run in the LVGL thread. Keys are documented in `protocol/README.md` (`ha/<entity>`, `weather`, `sys/wifi`, `sys/ha`, `sys/pc`).

**Threading rule.** LVGL is only touched from the LVGL thread: the esp_lvgl_port task on the device, the main loop in the simulator. Other threads only use `bus.post()`, `App::set_layout()` (validates, saves, then queues the rebuild) and the thread-safe `Settings` / `HaSession`.

**Layout → UI.**

- `Layout::parse` validates the JSON (4×4 grid, no overlap, required props) and returns French error messages.
- `App` builds one `lv_tileview` tile per page, with a grid layout, and asks `find_widget_factory(type)` (`core/src/widgets/registry.cpp`) for each widget.
- A type without a factory (layout from a newer editor) renders a "Non pris en charge" placeholder.
- Rebuild order matters: `close_sheet()`, then `bus.clear_subscriptions()`, then `widgets_.clear()`, then delete the LVGL objects.

**Widgets** (`core/src/widgets/`) subclass `dm::Widget`, build under `root`, subscribe through `ctx.bus`, and call `ctx.services.ha_call` / `pc_action` / `pc_media` / `ha_fetch`.

- Keep the `WidgetContext&`: it is an `App` member.
- Don't call virtual methods from base constructors. `EntityTile::start()` exists for that reason.
- Popups use `App::open_sheet()` (top layer). They own themselves, are deleted on close, and unsubscribe with the token returned by `Bus::subscribe`.

**Theme (`core/src/theme.cpp`).** Three card styles, chosen by `theme.style` in the layout:

- `flat` (default, chosen by the user for speed): no wallpaper, plain dark screen gradient, opaque cards.
- `frozen`: opaque cards painted by `finish_cards()` with the average glass tint of the wallpaper under their resting place (plain color: subtle RGB565 gradients band without dithering).
- `glass`: translucent white gradients over the wallpaper. Looks best, ~2.5x slower to draw.
- Anything translucent over a large area is expensive on the device: keep cards, popups (`panel_opa()`, `backdrop_opa()`) and active states (`paint_active()`) opaque outside the glass style.
- The wallpaper is generated at boot from the layout's accent colors (RGB565 with Bayer dithering).
- Active state uses `LV_STATE_CHECKED` local styles.
- Fonts: Inter (Latin-1, French accents) and MDI icons, generated by `firmware/tools/gen_assets.mjs` into `core/src/fonts/`. Sizes: `dm_font_14/16/20/28/40/72` (72 = digits only) and `dm_icons_18/24/32/48/72` (72 = weather icons only).

**Home Assistant.** `dm::HaSession` (core) implements the WebSocket API protocol:

- auth, `subscribe_entities` limited to `Layout::ha_entities()`, merging of compressed diffs, `call_service`, and `config/entity_registry/list_for_display` for the editor;
- the transport lives in `main/ha_client.cpp` (esp_websocket_client, frame reassembly);
- the simulator replaces HA with `sim::MockHome`.

**PC link.** `dm::PcSession` (core) is the screen side of the companion protocol (`protocol/README.md`, "PC link"):

- one connection at a time; a 6-digit pairing code shown through the `sys/pc_pair` bus key, then a token kept in `Settings` (`pc_name`/`pc_token`, forgotten with `PUT /api/settings {"pc":{"forget":true}}`);
- `metrics` go to the `pc` bus key (`pc_metrics`, `pc_gauge` widgets); shortcut actions go out through `Services::pc_action`; failures come back as `sys/toast`;
- frames are queued under the lock and written after it (`flush()`): never call the transport or `Settings::set_pc` with the lock held (`set_pc` triggers `on_change`, which calls `forget()`);
- transports: `main/http_server.cpp` (`/api/pc`, esp_http_server WebSocket) and `sim/ws_server.cpp` (minimal WebSocket server on the HTTP port + 1). `--mock-pc` fakes a PC; `/sim/tap?x=&y=` taps in `--no-window` mode.

**Media.** The `media` widget shows `pc/media` + `pc/media_art` (from the companion) and falls back to an optional HA `media_player`. The companion reads the macOS now playing through the vendored `mediaremote-adapter` (`companion/vendor/`, compiled by `build.rs` into `src-tauri/resources/mediaremote/`, run with `/usr/bin/perl` because only entitled binaries may use MediaRemote since macOS 15.4), scales covers to 160x160 RGB565 and picks their dominant color. HA covers (`entity_picture`, JPEG) are downloaded by `Services::ha_fetch` and decoded by `core/src/cover.cpp` with LVGL's TJpgDec (`LV_USE_TJPGD`).

**Companion (`companion/`).** Tauri 2 menu bar app, static UI in `companion/ui/` (no bundler, `window.__TAURI__`). `link.rs` runs the reconnect loop and the protocol, `metrics.rs` samples with sysinfo (GPU: `ioreg` IOAccelerator on macOS, NVML on Windows), `actions.rs` runs `keys` (enigo, on the main thread; macOS needs Accessibility; digits use physical key codes so AZERTY works), `launch` and `url`. Its config (address, token, last working IP) is `config.json` in the app config folder. Names are resolved IPv4-only (`dns-lookup`): an any-family lookup of `desk-monitor.local` waits seconds for an IPv6 answer that never comes. The window is a menu bar overlay (`overlay.rs`): left click on the tray icon toggles it under the icon, right click keeps the menu, and it hides when it loses the focus. On macOS it is turned into a non-activating `NSPanel` (class swap, like the tauri-nspanel plugin): a regular window must activate the app to take the keyboard, and macOS then leaves the Space of a full screen app. So never call `set_focus()` on it, and avoid native dialogs such as `confirm()`, which blur the overlay and hide it. "Ouvrir l'éditeur" opens the screen's web editor in a regular window (`editor.rs`, at the IP the link answered on; the simulator's HTTP port is the link port - 1), and the app shows in the Dock and Cmd+Tab while it is open. wry does not implement `alert`/`confirm`/`prompt` on macOS (they return false/null silently): the web editor uses its own `ask`/`askText` (`web/src/components/Dialog.tsx`), never the native ones. The bundle is ad-hoc signed with a stable identifier (`signingIdentity: "-"`) so that macOS keeps the Local Network and Accessibility permissions across rebuilds.

**Web editor (`web/`).** It talks to the REST API (`core/src/api.cpp`). It is built into one HTML file and gzipped. At every firmware build, `main/select_web.cmake` copies `web/dist/index.html.gz` (or `main/www_fallback.html` when it is missing) to `web.bin`, which gets embedded. The server sends `Content-Encoding: gzip` when the bundle starts with the gzip magic bytes. Icon SVGs come from `web/src/generated/icons.ts`, which is generated too.

## Gotchas

- **Icons must be declared.** An icon name used anywhere (layout, widget code, editor) must be in `protocol/icons.json`, and the fonts must be regenerated. Unknown names show `alert-circle-outline`.
- **Display orientation** (verified on hardware). No mirroring at all: v1's hardware mirror (SDIR/MADCTL) and LVGL 180° rotation cancelled out. Never use `esp_lcd_panel_mirror` with mirror X: the RGB driver then copies pixel by pixel instead of memcpy. SDIR (0xC7) is only accepted in command2 bank 0.
- **Internal RAM is the scarce resource** (~190 KB free before Wi-Fi). LVGL and cJSON allocate from PSRAM (`main/psram_alloc.cpp`, `CONFIG_LV_USE_CUSTOM_MALLOC`); `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` stays off (107 KB of IRAM); Wi-Fi, the HTTP server task and the draw/bounce buffers need the rest. `ESP_ERR_HTTPD_TASK` or a Wi-Fi `no mem` at boot means something ate it.
- **Display speed** (measured on the device, full-screen redraw): ~35 ms for the flush into the PSRAM frame buffer, +16 ms for the wallpaper, +50 to 95 ms for the translucent cards, so swipes run at ~8-10 FPS. Tried without success: 16 MHz pclk (picture shakes and shifts), two LVGL draw threads (+1 FPS, cost the RAM the HTTP server needed), 40-line bounce buffers. Long CPU-bound work in `app_main` (e.g. a boot benchmark) also shifted the picture. Now: flat style + two PSRAM frame buffers in direct mode (`DOUBLE_BUFFER` in `main/display.cpp`) against tearing; swipes are smooth (~20 FPS) according to the user. `desk_monitor_sim --do bench=N` gives the same breakdown in the simulator (`App::benchmark`).
- **RGB data pins** are in esp_lcd order (B0..B4, G0..G5, R0..R4), little-endian. ESPHome swaps the bytes because its buffers are big-endian; we don't.
- **`lv_tileview_get_tile_active()` is NULL** until a tile is set explicitly. `build_pages()` sets tile 0.
- **Labels** that must stay on one line use `theme::single_line()`, because `LV_LABEL_LONG_DOT` alone wraps when the height is `LV_SIZE_CONTENT`.
- **C++ and IDF macros.** Several IDF/component config macros use designated initializers in an order that fails in C++ (e.g. `ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG`). Fill the structs field by field.
- **Backlight (GPIO38).** It floats after reset and lights the uninitialized panel (white flash). `bootloader_components/backlight_off` drives it low in the bootloader, and `display_show()` turns it on only after the first frame is drawn (wake from sleep redraws first too). After adding a bootloader component, delete `build/bootloader` once: the bootloader subproject does not rescan.
- **Touch filter (`filter_touch`).** Never change `point_num` in the GT911 `process_coordinates` callback: a count of 0 makes `esp_lcd_touch_get_data` fail and esp_lvgl_port aborts. The wake-up touch is ignored with `lv_indev_wait_release()` instead.
- **TJpgDec pixel order.** LVGL's copy outputs 24-bit pixels as B, G, R (`cover.cpp`).
- **WebSocket frame size.** Covers are ~68 KB of base64: `MAX_WS_FRAME` (96 KB) in `main/http_server.cpp` must stay above it.
- **esp_http_server WebSocket.** In ESP-IDF 5.5 the URI handler is not called for the handshake, only for frames: `pc_ws_handler` opens the `PcSession` on the first frame and keeps its id with `httpd_sess_set_ctx` (frame requests do not copy `req->sess_ctx` back to the session).
- **Secrets.** `settings.json` stores the Wi-Fi password and the HA token on LittleFS. `GET /api/settings` never returns them (`has_*` flags), and an empty secret in `PUT` keeps the stored value.
- **Toolchain.** `sdkconfig` is generated and ignored; edit `sdkconfig.defaults`, then delete `sdkconfig` and run `idf.py set-target esp32s3`. `managed_components/` and `dependencies.lock` are fetched by the IDF component manager.
