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
| `ha_toggle` | `entity`, `name`, `icon`, `rules` | Home Assistant. A tap toggles (nothing for a read-only `binary_sensor`/`sensor`); a long press opens the entity's popup (see below) |
| `ha_light` | `entity`, `name`, `icon`, `rules` | Home Assistant. A long press opens the color/white/brightness popup |
| `ha_sensor` | `entity` or `template`, `name`, `icon`, `unit`, `decimals`, `format` (`auto`, `number`, `duration`: e.g. a Proxmox uptime shown as "12 j 4 h"), `show_age`, `rules` | Home Assistant. `show_age` adds "il y a 5 min" (last change). A `template` (Jinja, rendered live by HA) replaces the entity |
| `ha_climate` | `entity` (`climate` or `water_heater`), `name`, `icon`, `rules` | Home Assistant. Shows the current temperature, the target and what the device does (`hvac_action`). A tap opens the thermostat popup (target dial, HVAC/operation mode, preset, fan mode). −/+ change the target on tiles of 3 columns or 2 rows; taps are grouped and sent 0.8 s after the last one. A `rules` text replaces the status line |
| `ha_cover` | `entity` (`cover`), `name`, `icon`, `rules` | Home Assistant. Status ("Ouvert à 40 %"); from 2 columns open/close buttons, plus stop from 3 columns or 2 rows. A tap opens the popup (buttons, position and tilt sliders when supported) |
| `ha_slider` | `entity`, `name`, `icon`, `rules` | Home Assistant. A slider for `input_number`/`number` (`set_value`), a light's brightness, a fan's speed (`set_percentage`), a media player's volume (`volume_set`) or a cover's position. Sent on release |
| `ha_select` | `entity` (`input_select`, `select`, or a `fan`'s presets), `name`, `icon`, `rules` | Home Assistant. Shows the current option; a tap opens the list (`select_option` / `set_preset_mode`) |
| `ha_list` | `name`, `mode` (`rows` / `badges`), `decimals`, `items` (1 to 12: `entity` or `template`, `name`, `icon`, `rules`, `tap_action`, `hold_action`) | Home Assistant. `rows`: icon, name and state per line (a tap toggles what can be toggled, opens the popup of thermostats, sliders and lists). `badges`: green/red pills and "N en panne" (service status). An item is fine when a `binary_sensor` is on (off for alarm classes: problem, smoke, moisture…), a switch-like entity is on, anything else is available; a rule's `active` overrides it (true = fine, false = problem) |
| `ha_history` | `entity`, `name`, `icon`, `hours` (1-168, default 24), `unit`, `decimals`, `min`, `max`, `color` | Home Assistant `history/history_during_period`, reduced to 96 points on the screen and fetched again every 5 minutes; the last point follows the live state |
| `ha_alarm` | `entity` (`alarm_control_panel` or `lock`), `name`, `icon`, `rules` | Home Assistant. State ("Armée (absent)", "Déclenchée !"), colored while armed. A tap opens the commands (disarm, arm home/away/night/vacation as supported; lock, unlock, open) with a keypad when the entity has a `code_format` (arming asks for it only with `code_arm_required`) |
| `ha_gauge` | `entity`, `name`, `min`, `max`, `unit`, `decimals`, `warn`, `crit` | Home Assistant |
| `shortcut` | `name`, `icon`, `color`, `action` | `ha` action over Wi-Fi, other kinds via the PC |
| `pc_metrics` | `name` (default: the PC's name) | PC companion |
| `pc_gauge` | `metric` (`cpu`, `ram`, `gpu`, `vram`, `disk`, `cpu_temp`, `gpu_temp`), `name`, `warn`, `crit` | PC companion |
| `media` | `entity` (optional `media_player`) | PC companion (now playing of any app), else the Home Assistant player |
| `printer` | `name` (default: the printer's model) | 3D printer of the settings (see "3D printer"). 2 rows: thumbnail, file, progress, end time, temperatures; 1 row: progress line. A tap opens the full-screen view: pause/resume, stop and start (a second tap confirms), light, temperatures, last printed files |

A long press on an entity tile, or a tap on `ha_climate`/`ha_cover`/`ha_slider`/`ha_select`, opens the popup of the entity's domain (`open_more_info()`, `firmware/core/src/widgets/controls.cpp`): lights (color, white, brightness), `climate`/`water_heater` (target, modes), `cover` (buttons, position, tilt), `fan` (on/off, speed, presets), `input_number`/`number` and `media_player` (a slider), `input_select`/`select` (the option list).

Every Home Assistant tile also accepts `tap_action` and `hold_action`, which replace what a tap or a long press does by default (toggle tiles: toggle / popup; sensors: nothing; the other tiles: popup). Each is an action like a shortcut's (`ha`, `keys`, `launch`, `url`), or `{"kind": "toggle" | "more_info"}` (on the tile's entity, or on `entity_id`), or `{"kind": "none"}`. Any action, shortcuts included, may add `"confirm": true` or `"confirm": "Ouvrir le garage ?"`: the screen asks before running it.

Locks and alarm panels: a toggle (tap of an `ha_toggle`, `toggle` action, row of an `ha_list`) on an alarm panel or a lock with a code opens the keypad instead; unlocking a lock without code asks for confirmation first (once, even when the action has its own `confirm`). When Home Assistant refuses a service call (wrong code, unknown service…), its message shows as a toast.

### Visibility

Any widget can take `visible_if` in its props: a condition like a rule's `when` (`entity`, `attr`, `is`, `not`, `above`, `below`). The widget shows only while it holds; its cell stays empty otherwise, and also until Home Assistant has sent the state. Without `entity`, it tests the widget's own entity.

```json
"visible_if": { "entity": "binary_sensor.fenetre_salon", "is": "on" }
```

### Templates

`ha_sensor` can show a Home Assistant template instead of an entity: `"template": "{{ states('sensor.salon') | float | round(1) }}"`. The screen subscribes to it with `render_template` (one subscription per distinct template, 16 at most per layout, 1000 characters each) and shows the result like a state, so `unit`, `decimals`, `format` and `rules` (on the result, `{state}`) apply. An error shows "—". `POST /api/ha/template {"template"}` renders a template once for the editor.

### Rules

Every Home Assistant tile except `ha_gauge` and `ha_history` accepts `rules` (`ha_list`: per item): up to 12 display rules, evaluated on the screen at every state change. The first rule whose `when` matches applies (`firmware/core/src/rules.cpp`).

```json
"rules": [
  { "when": { "is": ["off", "unavailable"] }, "text": "Hors ligne", "icon": "lan-disconnect", "color": "#F87171" },
  { "when": { "attr": "hvac_action", "is": "heating" }, "text": "Chauffe", "color": "#F97316", "active": true },
  { "when": { "entity": "binary_sensor.fenetre", "is": "on" }, "text": "Fenêtre ouverte" },
  { "when": { "above": 25 }, "text": "{state} °C", "color": "#FBBF24" },
  { "text": "En ligne", "color": "#4ADE80" }
]
```

- `when` (every field set must hold, missing = always): `entity` (another entity; default the widget's), `attr` (test an attribute instead of the state), `is` / `not` (value or list), `above` / `below` (strict, numeric values only).
- Effects: `text` (state text; for a sensor it replaces the value and the unit; placeholders `{state}`, `{name}`, `{attr.<name>}`; numbers get a decimal comma, "21,5"), `icon`, `color` (tints the icon and the text, or the card when active), `active` (forces or removes the colored "on" look of a toggle/light tile).
- A matching rule makes an `unavailable` entity look available, so `"is": "unavailable"` can show "Hors ligne".
- The entities named in conditions are subscribed too.

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
| `GET` | `/api/status` | `version`, `platform`, `ip`, `rssi`, `uptime`, `heap_internal`, `heap_psram`, `reset_reason` (device only: `power_on`, `panic`, `brownout`, `task_wdt`…) |
| `GET` | `/api/printer` | the `printer` bus value (see "3D printer"), `{"state": "off"}` without a printer |
| `GET` | `/api/ha/entities` | `[{"id", "name"}]` from the HA entity registry (`503` when HA is not connected) |
| `POST` | `/api/ha/template` | `{"template"}` → `{"result"}`, or `422 {"error"}` with Home Assistant's message (`503` when HA is not connected) |
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
  "security": { "pin": "" },
  "printer": { "model": "creality_cr10se", "host": "192.168.1.132", "port": 0 }
}
```

`printer.model` is an id of `protocol/printers.json` (empty: no printer; unknown ids are refused), `port` 0 means the driver's default port.

## Data bus keys (inside the firmware)

Data sources publish on the bus, and widgets subscribe to it (`firmware/core/include/dm/bus.h`).

| Key | Value | Producer |
| --- | --- | --- |
| `ha/<entity_id>` | `{"s": state, "a": {attributes}, "lc": last change (Unix seconds)}` | `HaSession` (`subscribe_entities`) |
| `weather` | `{"name", "current": {"t", "feel", "code", "day", "rh", "wind"}, "daily": [{"wday", "code", "tmin", "tmax", "pp"}]}` | Open-Meteo fetch |
| `sys/wifi` | `{"rssi", "ip"}`, plus `{"ap", "ap_ip"}` in setup mode | network |
| `sys/ha` | `{"state": "off" \| "connecting" \| "ok" \| "auth_error"}` | `HaSession` |
| `sys/pc` | `{"state": "connected" \| "pairing" \| "off", "name"}` | `PcSession` |
| `sys/pc_pair` | `{"code", "name"}` while a pairing code is shown, `{}` to hide it | `PcSession` |
| `sys/toast` | `{"text"}` | anything (e.g. a failed PC action, a service call refused by Home Assistant) |
| `pc` | `metrics` fields (see PC link), `{}` when the PC is gone | `PcSession` |
| `pc/media` | `media` fields (see PC link), `{}` when nothing plays or the PC is gone | `PcSession` |
| `pc/media_art` | `{"id", "w", "h", "data"}` (RGB565, base64) | `PcSession` |
| `pc/claude` | `claude` fields (see PC link), `{}` when the PC is gone | `PcSession` |
| `pc/claude_asks` | `{"asks": [...]}` pending Claude Code approvals (see PC link), `{}` when the PC is gone | `PcSession` |
| `pc/claude_detail` | `claude_detail` fields of the open detail view (see PC link), `{}` when the PC is gone | `PcSession` |
| `ha_tpl/<key>` | `{"s": result, "a": {}}`, or `{"s": "unavailable", "error"}`; `key` = FNV-1a hash of the template (`HaSession::template_key`) | `HaSession` (`render_template`) |
| `ha_hist/<entity_id>/<hours>` | `{"t0", "t1", "v": [96 numbers or null]}` | `HaSession::request_history` (asked by `Services::ha_history`) |
| `ha_art/<entity_id>` | same shape, decoded from the player's `entity_picture` (JPEG) | media widget, through `Services::ha_fetch` |
| `printer` | see "3D printer" | `PrinterSession` |
| `printer/files` | `{"recent": [...]}`, see "3D printer" | `PrinterSession` |
| `printer/thumb/<path>` | `{"id", "w", "h", "data"}` (64×64 RGB565, base64), decoded from the printer's PNG | `PrinterSession::request_thumbnail` (asked by `Services::printer_thumbnail`) |

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
claude {…}                       →                            (Claude Code sessions and usage, when they change)
claude_asks {asks}               →                            (pending approvals, when the list changes)
                                  ← claude_answer {id, decision} (allow, deny, or ask in the terminal)
                                  ← claude_view {view, id?}      (detail view opened, view null when it closes)
claude_detail {view, …}          →                            (data of the open view, when it changes)
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

### Claude Code

The companion follows the Claude Code sessions of the PC (CLI, IDE extensions, the desktop app's Code tab) and sends them to the screen. It reads three sources:

- the live session registry of Claude Code, `~/.claude/sessions/<pid>.json` (one file per running session);
- the transcripts, `~/.claude/projects/**/*.jsonl`, for the tokens, and the subagents' meta files (`<session>/subagents/agent-<id>.meta.json`: type, description, finished);
- the hooks it installs in `~/.claude/settings.json` (HTTP hooks to `http://127.0.0.1:47823`), for the approvals and the live state.

The plan limits come from the Claude Code status line (`rate_limits`) when the relay is installed, else from the desktop app's `plan-usage-history.json`. Chats of claude.ai and of the desktop app are not visible: only the plan limits count them.

`claude` is a full snapshot, sent after `welcome` and when something changes (once a second at most). Texts are ASCII (the screen fonts are Latin-1).

| Field | Meaning |
| --- | --- |
| `sessions` | up to 6, the most recent first: `{id, name, project, surface, state, since, tool?, model?, context, tokens}` |
| `sessions[].surface` | `cli`, `vscode`, `jetbrains`, `desktop` or `other` |
| `sessions[].state` | `working`, `waiting` (an approval or an answer is expected) or `idle` |
| `sessions[].since` | Unix time (s) of the last state change |
| `sessions[].context` | tokens in the context window (input + cache of the last answer) |
| `sessions[].tokens` | tokens used by the session (input + output + cache) |
| `sessions[].subagents` | optional: number of running subagents |
| `today`, `week` | `{tokens, cost}`, cost in US dollars, an estimate at API prices |
| `days` | 7 daily token totals, oldest first, today last |
| `limits` | optional: `{five_hour: {pct, resets_at?}, week: {pct, resets_at?}, at, source}`, `source` = `statusline` or `desktop`, `at` = Unix time of the reading |
| `desktop` | true while the Claude desktop app runs |

`claude_asks` lists the approvals Claude Code waits for, oldest first: `{asks: [{id, project, tool, detail}]}`, where `detail` is the command, the file or the URL (200 characters at most). The screen wakes up and shows the first one. `claude_answer {id, decision}` answers it: `allow`, `deny`, or `ask` when the user closed the popup without choosing (Claude Code then asks in the terminal at once). An approval that is answered in the terminal, or that waits more than about 100 s, leaves the list, and Claude Code then asks in the terminal as usual.

A tap on a session or on the usage or limits card opens a full-screen detail view. The screen sends `claude_view {view, id?}` (`view` = `session` with the session's `id`, `usage` or `limits`; `null` when the view closes). While a view is open, the companion sends `claude_detail {view, …}` at once, then whenever its data changes:

| `view` | Fields |
| --- | --- |
| `session` | the session's snapshot fields, plus `context_max` (200 000, or 1 000 000 past 200 000), `tokens {in, out, cache_read, cache_write}`, `total`, `cost`, `started` (Unix time of the first answer), `subagents [{type, description, running, tokens}]` (6 at most, running ones first) and `tools [{name, detail, status, at}]` (last 8 calls seen by the hooks, newest first; `status` = `run`, `ok`, `fail`, `denied` or `stopped`) |
| `usage` | `models` (up to 4 model families, the busiest first), `days` (30, oldest first: `{tokens, cost, m}`, `m` = tokens by family in the order of `models`, then the others), `hours` (24 token totals of today), `projects` (top 5 over 30 days: `{name, tokens, cost}`; a session counts for the project it started in), `today`, `week`, `month` (`{tokens, cost}`) |
| `limits` | `five_hour` and `week` as in `limits`, plus `pace` (percent used minus the percent of the window gone, when the window's start is known) and `estimated: true` when `resets_at` comes from the last big drop of the weekly use; `at`, `source`, and `history` (`[[t, five_hour_pct, week_pct]]` over 7 days, one reading per hour at most, from the desktop app's history and the status line readings the companion keeps in `claude-limits.json`) |

## 3D printer

The screen follows one 3D printer of the local network by itself, without the PC. The user picks the model in the editor (Settings → Imprimante 3D); the model names the protocol driver. Models and drivers are listed in `protocol/printers.json`, and `firmware/tools/gen_printers.mjs` turns that file into the tables of the firmware (`core/src/generated/printers.inc`) and of the editor (`web/src/generated/printers.ts`).

| Driver | Printers | Transport |
| --- | --- | --- |
| `creality_ws` | Creality with the stock firmware (Creality OS): CR-10 SE (tested), K1 series, Ender-3 V3 KE | WebSocket `ws://<printer>:9999/`, no authentication. Thumbnails over HTTP on port 80 |

`PrinterSession` (`firmware/core/include/dm/printer.h`) owns the driver and publishes the common model. The platforms only carry the frames (`main/printer_client.cpp`, `sim/ws_client.cpp`) and download the thumbnails (`Services::printer_thumbnail`, one at a time).

`printer` (also `GET /api/printer`):

| Field | Meaning |
| --- | --- |
| `state` | `off` (no printer set), `connecting`, `offline`, `idle`, `printing`, `paused`, `finished`, `stopped`, `error` |
| `model`, `model_name` | catalog id and "Brand Model" |
| `caps` | commands the driver supports: `pause`, `resume`, `stop`, `start`, `light` |
| `file` | path of the current or last job on the printer |
| `name`, `material`, `estimate` | display parts of the file name: `deck_saver.stl_PLA_4h51m8s.gcode` gives "deck saver", "PLA" and 17468 s (`PrinterSession::describe_file`) |
| `progress`, `elapsed`, `remaining` | percent, seconds |
| `layer`, `layers` | when the printer reports them |
| `nozzle`, `nozzle_target`, `bed`, `bed_target` | °C, a target of 0 means "off" |
| `light` | chamber light, when the printer has one |
| `error` | "Erreur <code>" when the printer reports an error |

`printer/files` lists the last 12 printed files, newest first and without duplicates: `{"recent": [{"path", "name", "material", "estimate", "when", "duration", "result", "exists"}]}`, `result` = `done` or `failed` (stopped or failed), `exists` = false once the file is deleted from the printer (it cannot be started again). When a print ends, the screen shows a toast ("Impression terminée : …") and asks for the history again.

Commands (`Services::printer_command`, `PrinterSession::command`): `pause`, `resume`, `stop`, `start` with a file path, `light` with `on` or `off`. They return false when the printer is not connected or the driver lacks the command; the view then shows "Imprimante non connectée".

### Creality stock firmware (`creality_ws`)

Checked on a CR-10 SE without root (firmware of 2023, `model` F003):

- The printer pushes JSON objects by itself: the temperatures about every second (`nozzleTemp`, `bedTemp0`, as strings), the progress (`printProgress`, `printJobTime`, `printLeftTime` in seconds), and at connection a full object (`state`, `printFileName`, `targetNozzleTemp`, `targetBedTemp0`, `layer`, `TotalLayer`, `lightSw`, `err {errcode}`, fans…). Later objects only hold the fields that changed.
- `{"method":"get","params":{"reqGcodeFile":1}}` → `retGcodeFileInfo.fileInfo`: entries `dir:name:size:?:mtime:?:thumbnail` separated by `;`.
- `{"method":"get","params":{"reqHistory":1}}` → `historyList [{filename, starttime, usagetime, printfinish, …}]`, newest first.
- Thumbnails: `http://<printer>/downloads/humbnail/<file name>.png` (sic), 96×96 RGBA PNG. The history's own thumbnail paths answer 404.
- `{"ModeCode":"heart_beat"}` is answered with `ok` (Creality Print does; not seen in 12 s of listening).

Checked during a print (CR-10 SE):

- `state` 1 = printing, also during the heating and the bed calibration that come first (`printProgress` 0, `layer` 1, `printLeftTime` meaningless): the screen shows "Préparation" until the progress moves;
- `{"method":"set","params":{"pause":1}}` → `state` 5 within a second; `{"pause":0}` → back to 1 after about 40 s (the head goes back and the print continues);
- `{"lightSw":0|1}` → `lightSw` echoed within a second;
- `state` 4 after a print stopped from the printer (`deviceState` 0, the progress stays);
- `{"opGcodeFile":"printprt:<path>"}` from `state` 4 → `state` 1 within 2 s, heating to the file's temperatures;
- `{"stop":1}` → `state` 4 and the heaters off. Sent during the bed calibration, it only applies once the calibration ends (about a minute later);
- `TotalLayer` stays 0 on this printer: the layer count is not shown.

Not verified yet (taken from reverse-engineered clients):

- `state` codes 0 (idle) and 3 (error);
- `printfinish`: 1 = done, anything else is shown with a red cross (`failed`).

