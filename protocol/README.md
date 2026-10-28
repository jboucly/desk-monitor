# Desk Monitor protocol

Contracts shared by the firmware (`firmware/`), the web editor (`web/`) and the PC companion (`companion/`).

| File | Content |
| --- | --- |
| `layout.schema.json` | JSON Schema of `layout.json`: pages, widgets, theme, shortcut actions |
| `icons.json` | MDI icons compiled into the firmware fonts and offered by the editor |

## Layout

A layout is a list of pages (1 to 12). Each page is a **4×4 grid**. Every widget sets its top-left cell (`x`, `y`, 0 to 3) and its span (`w`, `h`, 1 to 4).
Widgets cannot overlap or leave the grid. The firmware validates every layout again (`firmware/core/src/layout.cpp`). When a layout is rejected, the previous one stays on screen and the API returns the reason in French.

| Type | Main props | Data source |
| --- | --- | --- |
| `clock` | `show_date`, `show_seconds`, `card` | device time (SNTP) |
| `weather` | `mode` (`current` / `forecast`), `days` | Open-Meteo, over Wi-Fi |
| `ha_toggle` | `entity`, `name`, `icon` | Home Assistant |
| `ha_light` | `entity`, `name`, `icon` | Home Assistant. A long press opens the color/white/brightness popup |
| `ha_sensor` | `entity`, `name`, `icon`, `unit`, `decimals`, `format` (`auto`, `number`, `duration`: e.g. a Proxmox uptime shown as "12 j 4 h") | Home Assistant |
| `ha_gauge` | `entity`, `name`, `min`, `max`, `unit`, `decimals`, `warn`, `crit` | Home Assistant |
| `shortcut` | `name`, `icon`, `color`, `action` | `ha` action over Wi-Fi, other kinds via the PC |
| `pc_metrics` | `name` (default: the PC's name) | PC companion |
| `pc_gauge` | `metric` (`cpu`, `ram`, `gpu`, `vram`, `disk`, `cpu_temp`, `gpu_temp`), `name`, `warn`, `crit` | PC companion |
| `media` | `entity` (optional `media_player`) | PC companion (now playing of any app), else the Home Assistant player |

Shortcut actions (`action.kind`):

- `ha`: `domain`, `service`, optional `entity_id` and `data`. It is sent straight to Home Assistant.
- `keys`: `keys`, e.g. `"cmd+shift+4"`. Several chords are separated by spaces. `cmd` maps to ⌘ on macOS and to the Windows key on Windows.
- `launch`: `target`, an application name or path.
- `url`: `url`, opened by the PC's default browser.

The kind list is meant to grow. A future `shell` kind will have to be whitelisted, or confirmed on the PC side.

## REST API (served by the screen on port 80)

Every response is JSON. Errors look like `{"error": "message en français"}`.
When a PIN is set, every write (`PUT`, `POST`) needs the `X-DM-Pin` header.

| Method | Path | Body / answer |
| --- | --- | --- |
| `GET` | `/api/layout` | current layout |
| `PUT` | `/api/layout` | new layout. It is validated, saved, and the screen rebuilds without rebooting (`400` when invalid) |
| `GET` | `/api/settings` | settings. Secrets are replaced by `has_password`, `has_token` and `has_pin` |
| `PUT` | `/api/settings` | partial settings. An empty secret keeps the stored one. `security.clear_pin: true` removes the PIN |
| `GET` | `/api/status` | `version`, `platform`, `ip`, `rssi`, `uptime`, `heap_internal`, `heap_psram` |
| `GET` | `/api/ha/entities` | `[{"id", "name"}]` from the HA entity registry (`503` when HA is not connected) |
| `POST` | `/api/ota` | raw firmware image (`firmware/build/desk_monitor.bin`). The screen reboots on it |
| `POST` | `/api/reboot` | reboots the screen |
| `GET` | `/*` | the web editor (single gzipped page) |

Settings shape:

```json
{
  "wifi": { "ssid": "", "password": "" },
  "ha": { "url": "http://homeassistant.local:8123", "token": "" },
  "weather": { "lat": 48.857, "lon": 2.352, "name": "Paris" },
  "time": { "timezone": "CET-1CEST,M3.5.0,M10.5.0/3" },
  "display": { "brightness": 100, "sleep_minutes": 5 },
  "security": { "pin": "" }
}
```

## Data bus keys (inside the firmware)

Data sources publish on the bus, and widgets subscribe to it (`firmware/core/include/dm/bus.h`).

| Key | Value | Producer |
| --- | --- | --- |
| `ha/<entity_id>` | `{"s": state, "a": {attributes}}` | `HaSession` (`subscribe_entities`) |
| `weather` | `{"name", "current": {"t", "feel", "code", "day", "rh", "wind"}, "daily": [{"wday", "code", "tmin", "tmax", "pp"}]}` | Open-Meteo fetch |
| `sys/wifi` | `{"rssi", "ip"}`, plus `{"ap", "ap_ip"}` in setup mode | network |
| `sys/ha` | `{"state": "off" \| "connecting" \| "ok" \| "auth_error"}` | `HaSession` |
| `sys/pc` | `{"state": "connected" \| "pairing" \| "off", "name"}` | `PcSession` |
| `sys/pc_pair` | `{"code", "name"}` while a pairing code is shown, `{}` to hide it | `PcSession` |
| `sys/toast` | `{"text"}` | anything (e.g. a failed PC action) |
| `pc` | `metrics` fields (see PC link), `{}` when the PC is gone | `PcSession` |
| `pc/media` | `media` fields (see PC link), `{}` when nothing plays or the PC is gone | `PcSession` |
| `pc/media_art` | `{"id", "w", "h", "data"}` (RGB565, base64) | `PcSession` |
| `ha_art/<entity_id>` | same shape, decoded from the player's `entity_picture` (JPEG) | media widget, through `Services::ha_fetch` |

## PC link (WebSocket)

The PC companion (`companion/`) talks to the screen over the local network. The screen is the server.

- **Endpoint:** `ws://<screen>/api/pc` on port 80 (`desk-monitor.local`). The simulator listens on its HTTP port + 1, i.e. `ws://localhost:8081/api/pc`.
- **Discovery:** the screen announces the mDNS service `_desk-monitor._tcp` (TXT `path=/api/pc`).
- **Frames:** text frames, one JSON object each, with a `type` field.
- **One PC at a time:** a new connection replaces the previous one.
- **Liveness:** the companion sends `metrics` every second once welcomed, and `ping` every second before (e.g. while the user types the pairing code). The screen drops a paired connection after 6 s of silence, an unpaired one after 30 s.

Pairing happens once. The screen shows a 6-digit code, the user types it in the companion, and the screen returns a token. The companion stores the token and presents it on every connection. Only a paired PC receives shortcut actions. The editor can forget the paired PC (`PUT /api/settings` with `{"pc": {"forget": true}}`).

```
PC                                  screen
hello {name, os, version, token} →
                                  ← welcome {name}            (valid token)
                                  ← pair_required             (no or unknown token)
pair_start                       →                            (the screen shows a code, valid 2 min)
                                  ← pair_pending
pair {code}                      →
                                  ← error {code: "bad_code"}  (5 tries at most)
                                  ← paired {token}, welcome {name}
metrics {…}                      →                            (every second, once welcomed)
media_art {id, w, h, data}       →                            (when the cover changes, before its media)
media {title, artist, …}         →                            (when the now playing changes)
                                  ← media_cmd {cmd}           (toggle, next, prev)
                                  ← action {id, action}       (a shortcut was tapped)
action_result {id, ok, error?}   →                            (an error shows as a toast)
ping                             →
                                  ← pong
```

Errors look like `{"type": "error", "code": "bad_code" | "pair_expired", "message": "…"}`.

`metrics` fields. All are optional; send the ones the PC can read:

| Field | Unit |
| --- | --- |
| `cpu`, `ram`, `gpu`, `vram`, `disk` | percent (0-100) |
| `cpu_temp`, `gpu_temp` | °C |
| `ram_used`, `ram_total` | bytes |
| `net_rx`, `net_tx` | bytes per second |

`media` describes the system's now playing (any app: Music, Spotify, a browser…). An empty `media` (no `title`) means nothing plays.

| Field | Meaning |
| --- | --- |
| `title`, `artist`, `album` | texts (typographic punctuation replaced by ASCII: the screen fonts are Latin-1) |
| `app` | name of the playing app ("Plexamp", "Google Chrome") |
| `playing` | true while playing, false when paused |
| `duration`, `elapsed` | seconds; `elapsed` is the position when the message was sent, the screen moves it forward while playing |
| `color` | `#RRGGBB` dominant color of the cover (controls, progress bar) |
| `art` | id of the cover sent with `media_art` |

`media_art` carries the cover: `w` x `h` pixels (160 x 160) in RGB565 little-endian, base64 in `data` (about 68 KB). It is sent only when the cover changes. On macOS the companion reads the now playing with the vendored `mediaremote-adapter` (`companion/vendor/`).

`action` carries the shortcut's `action` object from the layout (`keys`, `launch`, `url`). The companion runs it and answers with `action_result`.
