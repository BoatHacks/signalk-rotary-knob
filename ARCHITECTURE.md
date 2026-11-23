# SignalK Rotary Knob Architecture

> DRAFT 1. Requirements live in [SPEC.md](SPEC.md), the screens and input rules in [docs/ui-design.md](docs/ui-design.md), the board
> in [docs/hardware.md](docs/hardware.md), the evidence behind the base choice in [docs/spike-espos.md](docs/spike-espos.md).
> Items marked **(verify)** are unchecked against a live server or the board.

## 1. Overview

One firmware image for the Elecrow CrowPanel 1.28" (ESP32-S3), built on **espOS** (Wi-Fi setup, NVS config, SignalK client,
web settings page) with our own knob application on top: board drivers, LVGL UI, a state model fed by SignalK, adapters that turn UI
actions into SignalK calls, and a USB composite device (CDC log + HID).

```
                    +-------------------- SignalK server (boat LAN) --------------------+
                    |  WS delta stream      REST v2 (autopilot, notifications)   plugin |
                    +--------^----------------------------^-----------------------------+
                             | espos_sk (WS, token)       | espos_sk_http (token)
   +-------------------------+----------------------------+---------------------------+
   |  ESP32-S3                                                                         |
   |                                                                                   |
   |   espOS: wifi/net, config (NVS), httpd + settings UI, event bus, health           |
   |      |                                                                            |
   |   knob_model  <---- SignalK subscriptions (stream task callbacks, copy + enqueue) |
   |      | owns marine_ui_snapshot_t, stale timeouts, connection -> system screen     |
   |      |  snapshot (every change, max 10 Hz)            ^ actions                   |
   |      v                                                |                           |
   |   marine_ui (LVGL 9.3) <--- input task (encoder, button, touch) via feed_*        |
   |      | action callback -> queue                                                   |
   |      v                                                                            |
   |   knob_actions (worker task) --> adapters: autopilot | anchor | dimmer | notify   |
   |                              \--> knob_hid (HID reports)   knob_power (dim, LEDs)  |
   |                                                                                   |
   |   board: display, backlight, PCNT encoder, button, CST816D, WS2812, rails         |
   |   knob_usb: TinyUSB composite (CDC logs + HID), 1200 baud -> ROM download         |
   +-----------------------------------------------------------------------------------+
```

Principles:
- **The UI never talks to the network and the network code never touches LVGL.** The model owns the snapshot, adapters own all
  SignalK API details. The seam between them is the `marine_ui_snapshot_t` / `marine_ui_action_t` pair that already exists in
  `ui/marine_ui_v5`.
- **Fail safe.** Losing the link, the server, or the knob never changes autopilot or anchor state (SPEC section 2). No action is
  sent while its inputs are stale, and nothing non-idempotent is retried.
- **espOS owns the plumbing, we own the product.** We do not fork espOS. Where it lacks something (display, encoder, HID,
  dimming) the code is ours, in separate components.

## 2. System Components

All new components live under `firmware/components/`. Public headers are the only coupling between them.

### 2.1 board
Pin map, power rails (GPIO1/2 high first), GC9A01 over SPI2, backlight PWM, PCNT quadrature (x4, 4 edges per detent), button,
CST816D (reset pulse, ghost-report filter, centre dead zone), WS2812 on GPIO48 via RMT. Promoted from `spike/main/board.c`.
Owns nothing above hardware: it returns steps, edges and touch points. Depends on esp_lcd, esp_driver_*.

### 2.2 lv_port
LVGL 9.3 port from the spike: two 19.2 KB DMA draw buffers, partial render, async flush, recursive mutex, the LVGL task, PSRAM
for allocations over 4 KB. Provides `lv_port_lock()/unlock()`. No encoder indev: `marine_ui` has its own semantic classifier
(single, double, long) and takes raw steps and button edges, so the LVGL group/indev path from the spike is dropped.

### 2.3 marine_ui
The UI package in `ui/marine_ui_v5`, moved here unchanged as a component. Draws every mode, system screen, the mode ring, alarm
screen and stale overlay, from a snapshot. Emits `marine_ui_action_t` through one callback. Not compiled on hardware yet, so
bring-up is a risk item (section 9).

### 2.4 input
One task, 5 ms period: reads `board` for encoder steps, button level and touch, debounces the button, then calls
`marine_ui_feed_encoder_step()`, `feed_button_edge()`, `feed_touch()` under the LVGL lock and stamps `knob_power`'s activity
time. Activity is tracked here, **not** through LVGL's inactivity counter, so the dimmer wakes on turn, push and touch alike
(closes SPEC question 7). The input task also applies the 5 s boot hold for reset-setup.

### 2.4a knob_net
Polls espOS (Wi-Fi state machine, SignalK token state, stream status) every 250 ms and reports one boot phase: starting,
setup portal, Wi-Fi, server search, pairing, token check, link, failed, online. It carries a short text for the splash
status bar and a progress value for the progress bar, and whether a SignalK token is stored. Polled, not event driven, so it
cannot interfere with espOS. The model maps the phase to a system screen: setup portal, pairing and failed have their own
screens, online is the mode screen. In the other phases a knob with a stored token shows its mode screen at once, under the
`CONNECTING` overlay until the first data and `LINK LOST` after a drop; a knob without a token keeps the splash (rule in
`knob_logic`, `knob_boot_show_mode`).

### 2.5 knob_model
The only owner of `marine_ui_snapshot_t`. Inputs, all copied and enqueued from callbacks (never blocking on the stream task):
- SignalK subscriptions (`espos_sk_subscribe`), values and `meta` (for `meta.timeout`).
- Poll results from the adapters (autopilot `GET`, anchor state if not on the stream).
- espOS events (network up, token pending/approved/denied, stream connected/lost) mapped to `marine_system_screen_t`
  (BOOT, WIFI_PORTAL, CONNECTING with `connect_step`, PAIRING, CANNOT_CONNECT, OPERATIONAL).
- `knob_usb` (host enumerated), `knob_power` (night palette from `environment.mode`).

It stamps `<name>_updated_ms` with the knob's clock at arrival and fills `<name>_timeout_ms` from `meta.timeout` or the
per-path default in config. Staleness stays the UI's job (it computes age from the pair). Pushes a snapshot to `marine_ui`
under the LVGL lock when something changed, at most 10 Hz, plus a 1 s heartbeat so ages advance. Decided deviation from SPEC 6.1:
age is measured from the knob's receive time, not the server's timestamp, because the knob has no trusted clock before NTP.
A value the server held for a while shows fresh when it arrives. Revisit if a boat's deltas lag in bursts (section 9).

### 2.6 knob_actions and adapters
`marine_ui` calls a single callback that does nothing but push the action on a queue (never blocks the LVGL task). The
**actions task** pops it and dispatches to the adapter for the action's domain. Adapters are the only code that knows SignalK
paths and endpoints, and they use `espos_sk_put()` for stream PUTs and `espos_sk_http_*` for REST (authenticated with the
espOS token, blocking, so on this task only):

| Adapter | Does |
|---|---|
| autopilot | v2 API: `engage`, `disengage`, `PUT target` (radians), status `GET` for state/mode/target. Device id from config, default `_default`. |
| anchor | Drives `hoekens-anchor-alarm` (decided 2026-11-23, installed on the boat): REST `POST /plugins/hoekens-anchor-alarm/dropAnchor` (arm, position from the knob's `navigation.position` subscription plus a circle zone), `setZone` (radius or move the drop point), `raiseAnchor` (disarm). Uses `espos_sk_http` with the knob's readwrite token. Ack through the Notifications API (to verify). Another plugin means another adapter. |
| dimmer | `espos_sk_put()` on the configured channel path. Each channel has a value form chosen on the settings page: ratio 0..1, percent 0..100, or on/off. The boat's real convention is checked on the bench (SPEC question 2). |
| notifications | Acknowledge via the server Notifications API, falls back to the plugin if the anchor alarm does not use it (**verify**). |

Rules every adapter follows: refuse when the knob is not ONLINE or the inputs it depends on are stale (answer with a UI error
instead), never retry `engage`, `disengage`, `arm` or `disarm`, report success or failure to the model so the UI shows the real
server state rather than assuming. `SET_CTS` is absolute (idempotent); relative adjusts are made in the UI from the current
target.

### 2.7 knob_power
Backlight level (`ACTIVE` 60 %, `DIM` 10 % defaults, configurable), idle timer from the input task's activity stamp, overrides
(never fully off while the autopilot is engaged or an alarm is active, alarm wakes the screen), day/night level from
`environment.mode`. Executes `MARINE_ACT_POWER_SET_BACKLIGHT` from the UI. Also owns the LED patterns
(`MARINE_ACT_ALARM_LED_PULSE` / `_SLOW_PULSE`, state colours, turn feedback, off in night mode) on a small timer task.

### 2.8 knob_usb and knob_hid
TinyUSB composite (esp_tinyusb 2.x): CDC for logs and one HID interface with three reports (keyboard, mouse, consumer control).
`knob_hid` turns `MARINE_ACT_HID_TURN/PRESS/NEXT_PROFILE` into reports for the selected profile: chartplotter (mouse wheel and
left button), media (volume up and down, push is mute), navigation keys (left and right arrows, push is Enter). Reports go through a
small queue and task, only while a host has enumerated the knob. Written 2026-11-23, compiled, **not tested against a host yet**. Wi-Fi and the model keep running in HID mode so alarms
still reach screen and LEDs. `usb_host_enumerated` feeds the snapshot, and the HID entry in the mode ring appears only while a
host has enumerated the knob. The 1200 baud open on the CDC port reboots into the ROM download mode for reflashing
(verified in the spike).

### 2.9 knob_config
Config schema in espOS's NVS config store, exposed on the espOS settings page: visible modes, autopilot id, dimmer channels
(name plus path, up to 6), anchor plugin target, steps, timeouts and per-path stale defaults, dim levels, HID profile, and
the SignalK server host override. Versioned schema with defaults, so a firmware update never fails on old NVS. Export/import is
open (SPEC question 10), the schema is plain JSON so it is cheap to add.

### 2.10 main
`app_main` order, chosen for time to first pixels:
1. `board_power_on()`, `board_display_init()`, backlight at the dim level, boot screen via the raw path.
2. `board_input_init()`, `lv_port_init()`, `marine_ui_init()` with `BOOT` snapshot.
3. `knob_usb`, `knob_power`, `knob_config`.
4. `espos_start()`, then `knob_model` and `knob_actions` once the espOS events can be received.
Spike measurements: app_main at 0.33 s, first frame 0.6 s, LVGL up 0.37 s after app_main, the PSRAM memory test costs 0.43 s
(disable `CONFIG_SPIRAM_MEMTEST`).

## 3. Data Models

- **Snapshot / action:** `marine_ui_snapshot_t` and `marine_ui_action_t` in `ui/marine_ui_v5/.../marine_ui.h` are the contract.
  Do not widen it without changing the UI package's handoff.
- **Config:** SPEC section 4. One JSON document in NVS (`knob` namespace), schema version field, written only by the settings page.
- **Credentials:** the SignalK token and server selection stay in espOS's NVS store. We read with `espos_sk_get_token()`; we do not
  copy or log it.

### 3.1 Path map (defaults, all overridable, all **verify** against the boat)

| Snapshot field | SignalK source | Stale default |
|---|---|---|
| `heading_deg` | `navigation.headingTrue` or `navigation.headingMagnetic`, chosen on the settings page (default true), rad -> deg. The UI labels the unit `°T` or `°M` accordingly (snapshot field `heading_ref`) | `meta.timeout` else 5 s |
| `ap_engaged`, `cts_deg` | autopilot v2 `state`, `target`, deltas on `steering.autopilot.*` | 5 s |
| `rudder_deg` | `steering.rudderAngle` | 5 s |
| `depth_m` | `environment.depth.belowTransducer` | 10 s |
| `voltage_v` | configured battery path, `electrical.batteries.*.voltage` | 30 s |
| `gps_3d_fix` | `navigation.gnss.methodQuality` or position age | 10 s |
| anchor data | `navigation.anchor.state`, `.currentRadius`, `.maxRadius`, `.distanceFromBow`, `.bearingTrue` (rad), `.position` (hoekens-anchor-alarm) | 10 s |
| `active_alarm` | `notifications.navigation.anchor`, `notifications.*gps*` | event driven |
| dimmer levels | configured `electrical.switches.*` paths | 0 (never) |
| `night_palette` | `environment.mode` (`"day"`/`"night"`) | event driven, Day until first value |

## 4. Technology Stack

| Piece | Choice | Why |
|---|---|---|
| Language / SDK | C, ESP-IDF 6.1.x | espOS requires `>=6.1.0,<6.2.0`. Pin the exact version in docs. |
| Base framework | espOS 0.32.x (`espos_core`, `espos_sk`, `espos_ota`) | Wi-Fi portal, NVS config, SignalK client, settings UI, event bus. Pre-1.0 and single maintainer, hence the pinned lock file and the gate in section 9. |
| UI | LVGL 9.3.x, `marine_ui` | Round 240x240, canvas-drawn gauges and icons. |
| Display / touch | `esp_lcd_gc9a01` 2.x, own CST816D driver | Both from the spike. |
| USB | `esp_tinyusb` 2.x | Composite CDC + HID. Takes the native USB pins from USB-Serial/JTAG. |
| Memory | 8 MB octal PSRAM at 80 MHz, LVGL on malloc | LVGL plus espOS boot-looped without it. |
| Flash | 16 MB, espOS 16 MB table: two 6.5 MB app slots, 1.75 MB LittleFS | Verified in the spike. Kept so OTA can come back (section 8). |
| Tests | Unity host tests in `test/host` for the pure logic in `knob_logic` (value parsing, stale rule, steering and night fade, GPS-lost rule), built with a plain host C compiler; a hardware checklist for the rest | The logic that can hurt has no hardware dependency. Run on 2026-11-23 with gcc 14 on an aarch64 Debian host (the boat Pi, the dev machine has no host compiler): 11 tests, 0 failures, no warnings with -Wall -Wextra. |

## 5. Integration Points

- **SignalK server (WS and REST, token):** espOS discovers it (mDNS, manual host fallback), runs the access request and holds
  the token. The knob subscribes and PUTs through `espos_sk`, and does REST through `espos_sk_http`, which sets the bearer
  token, follows the server scheme and reports 401/403 back to espOS so it re-requests. It must never be called from the stream
  task or an espOS event handler (it blocks), so it runs on the actions task only. This answers SPEC question 8: yes, authenticated
  HTTP is available, no hand-rolled `esp_http_client`.
- **Autopilot provider plugin:** only through the v2 autopilot API.
- **Anchor alarm plugin:** the adapter encapsulates the plugin; swapping `hoekens-anchor-alarm` for another plugin means a different adapter
  only.
- **USB host:** HID (profiles) and CDC. No data protocol over CDC, it carries logs only.
- **Boat network:** plain WS/HTTP on the LAN, no TLS (SPEC section 12).

## 6. Security Considerations

- Trust model: a closed boat LAN, the device holds one credential (the SignalK token) in NVS. Nothing else is secret.
- **Token permission:** the access request grants whatever the server's admin approves. The adapters need write access for
  `PUT` and the autopilot and anchor calls. If the server grants read-only the knob must show an error, not fail silently
  (SPEC question 9).
- **Setup portal is an open AP and espOS's REST API has no key until `httpd.api_key` is set.** Decided: the setup AP closes as
  soon as Wi-Fi works (it reopens only when unconfigured or after the long-hold reset), and the API key is a fixed key the
  owner enters on the settings page, no generated secret shown on the screen. Whether the portal can take the key in the same
  step as the Wi-Fi credentials is unchecked (**verify**). Until it can, a knob has an open window at first setup. Treat this as a
  release blocker, not a polish item.
  Findings 2026-11-23: espOS closes the setup access point by itself once Wi-Fi works (policy auto, verified on the bench: the
  portal reports inactive while connected). For the key, espOS has the build option `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED`: protected
  endpoints answer 403 until a key is set. A product build must turn it on (a commented line in `firmware/sdkconfig.defaults`).
  It stays off on the bench, because with Wi-Fi configured the portal is not up to set the key, and the way back in is the portal
  recovery window (3 power cycles in a row).
- No logging of tokens, Wi-Fi passwords or full request headers, including on the CDC port.
- USB: anyone with physical access to the cable can reboot into ROM download mode. Same exposure as the BOOT button, accepted.
  Secure boot and flash encryption are off for the MVP.
- Inputs from the network (SignalK values, `meta`) are bounded (string lengths, array sizes) before they reach the snapshot,
  and every adapter reply is size-limited by `espos_sk_http`'s `max_body`.
- Action safety lives in two places on purpose: the UI requires the long press plus confirm turn, and the adapter refuses to send
  when its inputs are stale or the link is down.

## 7. File Structure

```
signalk-rotary-knob/
  SPEC.md  ARCHITECTURE.md
  docs/            hardware.md  spike-espos.md  ui-design.md  HANDOFF.md
  firmware/
    CMakeLists.txt  sdkconfig.defaults  partitions.csv  dependencies.lock
    main/                       app_main, boot order, glue
    components/
      board/  lv_port/  input/  knob_net/  knob_model/  knob_actions/ (the adapters and their worker task)  knob_logic/ (pure logic)  knob_power/
      knob_usb/ (+ knob_hid)  knob_config/
      marine_ui/                from ui/marine_ui_v5/esp_idf_component
    test/                       Unity tests, linux target
  ui/marine_ui_v5/prototype/    browser prototype stays as the design reference
  tools/                        flash and capture helpers (no machine paths)
```

`spike/` is promoted with `git mv` to `firmware/`, then refactored. The spike test screen (`app_ui.c`) is deleted when `marine_ui`
replaces it. `ui/marine_ui_v5/esp_idf_component` moves to `firmware/components/marine_ui`, the prototype stays under `ui/`.

## 8. Deployment

- **Build:** ESP-IDF 6.1.x, `idf.py -B <build dir outside the tree> build`. The espOS build needs an app signing key
  (`espsecure generate-signing-key --version 2 --scheme rsa3072`), kept outside git. A lost key means a USB reflash, nothing
  more.
- **Flashing (MVP):** over USB. The native USB pins belong to TinyUSB at runtime, so a running knob re-enters the ROM
  download mode when the CDC port is opened at 1200 baud (the firmware does this, `tools/` has the script). The BOOT button is the
  fallback. A blank board flashes over USB-Serial/JTAG as usual.
- **Updates over the air:** post-MVP. The 2-slot table, app rollback and the signing key stay in the build so OTA can be added
  without a re-partition. Signed firmware repository and update source are post-MVP, nothing in the MVP points `espos_ota` at a
  server (this replaces the earlier "never OTA" decision, SPEC sections 10 and 12).
- **First run:** setup AP, Wi-Fi credentials on a phone, SignalK access request approved in the server admin UI, channels and
  anchor target set on the settings page.
- **Runtime requirements:** 5 V supply, a SignalK server with security on (the knob is a normal device client), the autopilot
  and anchor plugins configured.
- **Persistence:** NVS holds espOS settings, token and the knob config. Nothing else is written, so power loss is safe.
- **USB identity:** the spike uses the Espressif VID with a throwaway PID, fine for the bench. Before anyone else uses this
  firmware it needs its own VID/PID.
- **Licence:** Apache-2.0 (decided), matching the espOS and cockpit code we reuse. A `LICENSE` file and per-file headers on new
  files go in with the promotion to `firmware/`.

## 9. Spike Gate, Risks and Open Items

**Base decision (2026-11-22): espOS, gated.** The gate is the remaining spike measurements. If any fail, the fallback is
espOS components used selectively (`espos_sk`, `espos_config`, `espos_net`) under our own `main`, then plain ESP-IDF.

| Gate | Pass if | Status |
|---|---|---|
| Wi-Fi join and SignalK access request through the espOS portal | Token approved once, survives reboot | **passed 2026-11-23**: joined Wi-Fi, access request sent to the boat server (manual host through a tunnel), approved with readwrite, delta stream connected. The token survived a reboot and a reflash |
| Boot to `SK_STREAM_CONNECTED` | ONLINE under 10 s, last screen under 4 s | **ONLINE passed 2026-11-23**: 5.2 s after boot by the chip clock (Wi-Fi joining from 2.4 s, server 3.4 s, token check 4.7 s, link 5.0 s, online 5.2 s; manual server). **Last screen under 4 s**: decided 2026-11-23 to open the last mode as soon as espOS reports a stored token, under a `CONNECTING` overlay until the stream brings data (docs/ui-design.md section 4). **Passed 2026-11-23**: the last mode opens about 2.5 s after power on. Separate finding: the server closes the stream about 3 s after the first connect on every boot, the knob reconnects 1.5 s later (hardware checklist, "Stream stays up after boot") |
| espOS alive next to USB composite | setup AP or stream up while CDC+HID enumerated, no health reboot | unconfirmed |
| Stream and REST under LVGL load | no dropped frames above 2 per minute during turning and a heartbeat | untested |
| `espos_sk_http` autopilot `GET` against the real server | 200 with token | untested |

**Implementation order (decided): UI bring-up first.**
1. Promote `spike/` to `firmware/`, split into components, add the Apache-2.0 licence.
2. Compile `marine_ui` on the panel with the demo snapshot, input task feeding it, power and LEDs. Fix what the first run shows.
3. Wi-Fi and token through the portal, measure boot to stream (the remaining gates).
4. `knob_model` and the dimmer adapter (thin vertical slice), then autopilot, then anchor.
5. USB-HID mode, config export/import, hardening items.

Risks:
- `marine_ui` has never been compiled or run on this panel (curved text, icon arcs, frame time of the tick ring). Plan a
  bring-up phase with the demo snapshot before wiring SignalK.
- espOS is pre-1.0. Pin versions, keep `dependencies.lock`, no forks.
- Setup AP and REST API are open until configured (section 6).
- Knob-clock staleness (section 2.5) may be wrong on a bursty network.
- CST816D ghost touches and the push-as-touch coupling: filtered and dead-zoned, confirmed only partly on the bench.

Open items carried from SPEC section 13: dimmer path convention (2), anchor plugin and ack route (3), `meta.timeout` coverage (11),
config export/import (10), token permission (9). The wake-from-dim check and PRESSED-on-screen check still need a person at the board.

## 10. Future Considerations

- OTA with a signed firmware repository (partition table and signing key already in place).
- A companion plugin if shims, snapshots or central config turn out to be needed (adapters can point at it).
- Touch masking around pushes, so the centre can be used for touch again.
- Haptics, a second knob, a custom enclosure and mounting.
- Config export/import and a second-knob clone path.
- Config export/import as JSON from the settings page (planned, after the MVP works).
- Own USB VID/PID before the repo goes public.
