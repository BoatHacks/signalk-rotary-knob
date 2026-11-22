# SignalK Rotary Knob Specification

> DRAFT 1. Items marked **(verify)** are assumptions to check against the SignalK docs or on the bench.

## 1. Introduction

### 1.1 Purpose

A helm-side control surface for a SignalK boat, built on the Elecrow CrowPanel 1.28" rotary display
(see [docs/hardware.md](docs/hardware.md)). One round screen, one endless dial, one push button, touch.
It controls three things:

- **Autopilot:** engage, standby, mode, adjust target heading.
- **Dimmer:** brightness of lighting circuits or display groups.
- **Anchor alarm:** arm, disarm, set the drop point and radius, see drag state, acknowledge alarms.

The project has two parts: this firmware, and a companion SignalK server plugin that owns pairing,
configuration, and the mapping from knob actions to SignalK paths. Firmware stays generic.

### 1.2 Background

- Knob hardware is a USB/Wi-Fi ESP32-S3 device with no battery, powered from a 5 V supply.
- Autopilot control goes through the SignalK autopilot API so any autopilot with a provider plugin works **(verify API shape)**.
- Part of the BoatHacks SignalK plugin suite.

### 1.3 Terminology

- **Knob**: the physical device running this firmware.
- **Companion plugin**: the `signalk-rotary-knob` SignalK server plugin.
- **Mode / screen**: one UI context (Autopilot, Dimmer, Anchor, ...). The dial and button mean different things per mode.
- **Transport**: how the knob reaches its target. `wifi` (talks to the plugin) or `hid` (acts as a USB input device for a host).
- **Dim mode**: low-power idle state with the backlight and LEDs reduced.

## 2. Domain Rules

- Autopilot safety first: engaging, disengaging and large heading changes must never happen from a single
  accidental dial turn. Engage is a long press followed by a confirm turn of the dial. Target changes apply in
  fixed steps (1 and 10 degrees).
- Anchor actions that change state (arm, disarm, set drop point, change radius) use a simple press-to-confirm,
  without the confirm turn.
- The knob never owns alarm logic. The server decides when an anchor alarm fires. The knob shows it and
  can acknowledge it.
- A stale connection must be visible. If the knob has no live link, controls are disabled and the screen
  says so. It must never show old autopilot or anchor data as if it were current.
- Fail safe: losing the knob (unplugged, Wi-Fi gone) never changes autopilot or anchor state.

## 3. State / Lifecycle Model

### 3.1 Connection states

`BOOTING -> CONNECTING -> PAIRING (first time only) -> ONLINE -> STALE -> CONNECTING ...`

- **ONLINE**: link up and fresh data received within a timeout.
- **STALE**: link up or down but data older than the timeout. Controls disabled.

### 3.2 Power states

`ACTIVE -> DIM -> ACTIVE`. Dims after an idle timeout. Any input (turn, press, touch) or an alarm wakes it.
While the autopilot is engaged or an alarm is active, the display never goes fully off, it only drops to a low
level so heading and alarm state stay visible. The dim level also follows the boat's night/day mode and
`environment.sun`, not just idle.

## 4. Data Model

- **Pairing**: device id (from the MAC), server address, device token. Stored in NVS.
- **Layout**: ordered list of modes the plugin wants shown, with per-mode settings (which autopilot id,
  which dimmer channels, anchor plugin target).
- **State snapshot** (plugin to knob): autopilot state/mode/target/heading, dimmer channel levels,
  anchor state (armed, position, radius, distance, alarm state).
- **Action** (knob to plugin): `{mode, action, value}`, for example `autopilot.adjustTarget +10`,
  `dimmer.set channel=salon 40`, `anchor.arm`.

## 5. Sources / Inputs

- Physical: dial (quadrature), push (short, long, double), capacitive touch (swipe to change mode, optional).
- Network: state snapshots and config from the companion plugin over Wi-Fi.
- Day/night: night/day mode or `environment.sun` from SignalK, relayed in the state snapshot **(verify paths)**.
- USB: host enumerates the knob. Wired power from the same cable.

## 6. API Specification

### 6.1 Knob to plugin (Wi-Fi)

Draft shape, to be settled in ARCHITECTURE.md:

- WebSocket from knob to plugin. Plugin pushes `state` and `config`, knob sends `action` and `hello`.
- Pairing: plugin shows a pairing code in its config page, knob sends it, plugin returns a token.
- Security: plain WebSocket on the boat LAN, every connection authenticated by the pairing token. No TLS.
- mDNS discovery of the SignalK server (`_signalk-ws._tcp`) **(verify)**, manual host as fallback.
- The plugin translates actions to the SignalK autopilot API, PUTs on `electrical.switches.*` dimmer paths, and the anchor alarm API.

### 6.2 USB-HID

A selectable mode: one transport per mode. SignalK modes use the Wi-Fi link, HID mode drives the host, and Wi-Fi
stays connected in the background in HID mode so alarms still reach the screen and LEDs. The knob enumerates as a composite device (HID dial and/or mouse wheel, media keys,
plus CDC for logs) **(verify composite works in TinyUSB mode on this board)**. Used to drive a host
application such as OpenCPN. The profile is configurable per mode (dial, mouse wheel, media keys) and
defaults to a HID dial.

### 6.3 SignalK interfaces the companion plugin uses

Researched against the SignalK server docs and the anchor alarm plugin README. The knob never talks to these directly.

- **Autopilot (v2 API)**, base `/signalk/v2/api/vessels/self/autopilots/{id}` (`_default` for the default device):
  `GET` status (`state`, `mode`, `target` in radians, `engaged`), `POST engage`, `POST disengage`,
  `PUT state`, `PUT mode`, `PUT target` and `PUT target/adjust` with `{"value": -2, "units": "deg"}`.
  Deltas arrive on `steering.autopilot.*`. An unreachable device reports `off-line`.
- **Anchor alarm** (plugin id `anchoralarm`, `POST /plugins/anchoralarm/...`): `dropAnchor {radius}`, `setRadius`,
  `raiseAnchor`, `setAnchorPosition`. Also PUT handlers on `navigation.anchor.position` (null raises),
  `navigation.anchor.maxRadius`, `navigation.anchor.rodeLength`. No documented arm/disarm endpoint, so arming
  is implied by position plus radius. Disarm maps to `raiseAnchor`.
- **Alarm ack/silence**: documented only for the server Notifications API
  (`POST /signalk/v2/api/notifications/{id}/acknowledge` and `/silence`). Unconfirmed whether the anchor plugin's
  alarms go through it.
- **Day/night**: `environment.mode` exists but its values are undocumented. `environment.sun` is not in the 1.7.0 spec.
- **Dimmer**: no `electrical.switches` dimmer convention found in the spec. To settle against a real server.
- **Server auth and discovery**: device access request flow (`POST /signalk/v1/access/requests`, poll for token),
  `Authorization: Bearer` on the WebSocket, mDNS `_signalk-ws._tcp` / `_signalk-http._tcp`.

## 7. User Interface

- 240x240 round, readable in direct sun and at night (night palette, red-shifted, dim mode).
- Dial turns the focused value, press confirms or toggles focus, long press opens the mode ring,
  touch swipe also switches modes.
- 5 LEDs: ring feedback, for example autopilot engaged, anchor armed, alarm flashing red.
- One glanceable home per mode, no menu diving at the helm.

## 8. Persistence

NVS: pairing/token, server host, last mode, brightness limits, HID/Wi-Fi mode selection.
Everything else is ephemeral.

## 9. Configuration

- Via the companion plugin config page (layout, channels, steps, timeouts, dim levels).
- First-time Wi-Fi: the knob opens a captive portal AP on first boot or after a long-press reset. SSID and the
  plugin pairing code are entered on a phone.

## 10. MVP Scope

### 10.1 MVP Features

- Wi-Fi connect, pair with the plugin, auto reconnect, STALE handling.
- Autopilot mode (engage/standby, target +/-1 and +/-10).
- Dimmer mode (one channel group, then several).
- Anchor mode (arm/disarm, set drop point and radius, state, ack).
- Always-on with dim mode.
- Boot to last screen under 4 s and ONLINE under 10 s (bench-measure, tighten later).

### 10.2 Post-MVP / Deferred

- USB-HID dial mode (second milestone, after Wi-Fi control works).
- Any over-the-air update path. Decided: firmware is NOT pulled from the companion plugin. Flashing is over USB.
- Haptics, battery, custom enclosure and mounting.

## 11. References

- [Elecrow GitHub examples](https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen)
- [docs/hardware.md](docs/hardware.md)

## 12. Design Decisions

- **Wi-Fi via companion plugin, not direct to SignalK.** Keeps the firmware generic and puts pairing, layouts and
  path mapping in a place that is easy to change without reflashing.
- **USB-HID is a selectable mode, not a separate firmware.** Both transports share the input and UI code.
- **Any autopilot through the SignalK autopilot API.** No autopilot specific code in the knob.
- **Full anchor control on the knob** (arm/disarm and drop point), alarm logic remains on the server.
- **ESP-IDF + LVGL**, for control over PCNT, boot time, OTA, power and the esp_tinyusb composite device.
- **Engage = long press + confirm turn.** Anchor changes only need a press, because they are less time critical.
- **Dimmer = `electrical.switches` dimmer paths**, with channels mapped by the plugin.
- **Build on espOS (ESP-IDF 6 components, `signalk-espos/espos_core`) with our own knob app.** espOS provides the Wi-Fi
  setup portal, NVS config, SignalK discovery/token/stream/PUT, REST/web UI, event bus and logs. It has no
  display, encoder, HID or dimming support, so those stay ours. We borrow ideas and code (Apache-2.0, 2.x line only)
  from `dirkwa/espos-p4-cockpit`: the `cockpit_hal` display/touch HAL split, `IdleDimmer` with wake overlay, `sun_state`
  and the day/night theme. We do not adopt its touch-first JSON Layout Player for the MVP: our UI is a fixed set of
  modes driven by the encoder. espOS is pre-1.0 and S3/CrowPanel support is unverified, hence the bench spike
  ([docs/spike-espos.md](docs/spike-espos.md)).
- **Companion plugin stays as specified** (pairing, layouts, channel mapping, path shims) even though espOS's own SignalK
  client could talk to the server directly. See open questions 5 and 6 for the overlap with espOS.
- **Day/night follows `environment.mode`** (`"day"` / `"night"`), as `sun_state` in the cockpit firmware does. Default to Day until the first
  value arrives, because a dark helm display is worse than a bright one.
- **Plain WS plus pairing token, no TLS.** Keeps connect time inside the ONLINE target on a closed boat LAN.
- **No firmware updates pulled from the plugin.** Decided after first leaning the other way. Flashing over USB.
- **Configurable HID profile, default dial.**
- **Looser boot targets (4 s / 10 s)** because the device is always on, not cold-booted per use.

## 13. Open Questions

1. Night/day: `environment.mode` with `"day"`/`"night"` is what the espOS ecosystem uses. It is published by
   `signalk-espos-manager` (from sun altitude) or `signalk-derived-data`. Confirm one of them runs on the boat.
2. Dimmer paths: which `electrical.switches.*` convention does the boat actually use, and how is it PUT?
3. Anchor alarm: the espOS cockpit docs target `hoeken/hoekens-anchor-alarm` (PUT `null` on `navigation.anchor.position`
   raises, PUT `navigation.anchor.maxRadius` sets scope, dropping at the current fix needs a plugin change). Which plugin
   do we drive? Previously researched: sbender9 `anchoralarm`. Is the `anchoralarm` plugin the target? How are its alarms acknowledged, and does disarm equal `raiseAnchor`?
4. How is firmware updated if not over the air from the plugin? USB flashing is assumed for now. Does the
   partition table still keep a second app slot? espOS ships signed OTA with rollback and a plugin-served manifest
   option. Decide whether we keep OTA disabled, or only allow a manually posted URL.
5. Pairing overlap: espOS already does the SignalK access request and keeps the token in NVS. What does our plugin's pairing
   code add, and which side owns the layout and channel config (espOS settings page vs the plugin)?
6. Does the knob subscribe and PUT directly with espOS's SignalK client for simple paths (dimmer, anchor radius), with the
   plugin only for the autopilot and anchor shims, or does everything go through our own knob protocol?
7. Can the encoder indev wake the dimmer? In the cockpit code LVGL's inactivity counter is reset by the touch indev. Our
   encoder and button need to do the same.
