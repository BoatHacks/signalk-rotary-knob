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

The project is this firmware. It talks to the SignalK server directly, there is no companion plugin in the MVP (section 12).
Pairing, configuration and the mapping from knob actions to SignalK paths and API calls live in the firmware and on the knob's
own settings page. A plugin may be added later (section 6.1).

### 1.2 Background

- Knob hardware is a USB/Wi-Fi ESP32-S3 device with no battery, powered from a 5 V supply.
- Autopilot control goes through the SignalK autopilot API so any autopilot with a provider plugin works **(verify API shape)**.
- Part of the BoatHacks SignalK plugin suite.

### 1.3 Terminology

- **Knob**: the physical device running this firmware.
- **Companion plugin**: a possible later SignalK server plugin for shims, snapshots or central config. Not part of the MVP.
- **Mode / screen**: one UI context (Autopilot, Dimmer, Anchor, ...). The dial and button mean different things per mode.
- **Transport**: how the knob reaches its target. `wifi` (talks to the SignalK server) or `hid` (acts as a USB input device for a host).
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

- **Pairing**: device id (from the MAC), SignalK server address, device token from the SignalK access request. Stored in
  NVS by espOS. This is the only credential the knob holds, there is no separate knob-to-plugin pairing.
- **Config** (on the knob, in NVS, edited on the espOS settings page): ordered list of visible modes, per-mode settings
  (autopilot id, dimmer channels as a name plus a SignalK path, anchor alarm plugin target), steps, timeouts, dim levels and
  the HID profile.
- **Local state model**: the knob keeps its own model, filled from SignalK subscriptions: autopilot state/mode/target/heading,
  dimmer channel levels, anchor state (armed, position, radius, distance, alarm state). Every value carries the time it last
  arrived, so the UI can decide what is stale.
- **Action** (UI to adapter layer): `{mode, action, value}`, for example `autopilot.adjustTarget +10`,
  `dimmer.set channel=salon 40`, `anchor.arm`. The firmware turns an action into a SignalK `PUT` or REST call (section 6.1).

## 5. Sources / Inputs

- Physical: dial (quadrature), push (short, long, double), capacitive touch (swipe to change mode, optional).
- Network: SignalK stream subscriptions and REST calls to the server over Wi-Fi.
- Day/night: night/day mode or `environment.sun` from SignalK, subscribed directly **(verify paths)**.
- USB: host enumerates the knob. Wired power from the same cable.

## 6. API Specification

### 6.1 Knob to SignalK (Wi-Fi)

Decided 2026-11-22: no companion plugin in the MVP, the knob talks to the SignalK server directly. Draft shape, to be settled
in ARCHITECTURE.md:

- espOS's SignalK client does discovery (mDNS `_signalk-ws._tcp` **(verify)**, manual host as fallback), the access request,
  the token, the WebSocket stream with subscriptions, and `PUT`.
- Pairing: one approval, the SignalK access request in the server's admin UI. No pairing code and no second token.
- Security: plain WebSocket and HTTP on the boat LAN, every request authenticated by the SignalK token. No TLS.
- Simple paths go through the stream and `PUT`: `environment.mode` subscription, dimmer paths, anchor radius.
- Autopilot v2 REST calls, anchor alarm endpoints and alarm acknowledge are plain HTTP requests with the token. Whether
  espOS's client can make them, or we use `esp_http_client` with the stored token, is open (question 8).
- The firmware keeps these in one adapter layer per domain (autopilot, anchor, dimmer, notifications), so SignalK API details
  stay in one place. If a plugin is added later for shims, snapshots or central config, the adapter can point at it.
- The token must be granted write access for `PUT` and the autopilot and anchor calls **(verify)** (question 9).
- Stale handling: each subscribed value has an age and a timeout. The timeout is the key's SignalK `meta.timeout` (seconds a
  value stays valid) when the server has one, otherwise a per-path default on the knob. `timeout: 0` or an event-driven key never
  times out by age. Age is measured against the server's timestamps, not the knob's clock. When a value the current mode
  depends on is stale, or the stream is down, the UI shows STALE and disables controls (section 3.1, docs/ui-design.md
  section 4).

### 6.2 USB-HID

A selectable mode: one transport per mode. SignalK modes use the Wi-Fi link, HID mode drives the host, and Wi-Fi
stays connected in the background in HID mode so alarms still reach the screen and LEDs. The knob enumerates as a composite device (HID dial and/or mouse wheel, media keys,
plus CDC for logs) **(verify composite works in TinyUSB mode on this board)**. Used to drive a host
application such as OpenCPN. The profile is configurable per mode (dial, mouse wheel, media keys) and
defaults to a HID dial.

### 6.3 SignalK interfaces the knob uses

Researched against the SignalK server docs and the anchor alarm plugin README. The firmware calls these through its adapter
layer (section 6.1).

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
- The full UI design is in [docs/ui-design.md](docs/ui-design.md). Summary: encoder first. Turn adjusts, single press
  (about 300 ms window) confirms or steps, **double press opens the mode ring**, long press is for state changes. Touch is
  secondary: never for autopilot or anchor, ignored within 50 px of the centre.
- 5 LEDs: alarm all red flashing, stale amber pulse, steady state colour (autopilot engaged, anchor armed), slow red pulse
  after an acknowledged alarm, turn feedback (off in night mode).
- One glanceable home per mode, no menu diving at the helm.

## 8. Persistence

NVS: pairing/token, server host, last mode, brightness limits, HID/Wi-Fi mode selection.
Everything else is ephemeral.

## 9. Configuration

- On the knob, through the espOS settings web page of each device (layout, dimmer channels and paths, anchor target, steps,
  timeouts, dim levels, HID profile). Stored in NVS. Each knob is configured separately, backup and restore is open
  (question 10).
- First-time Wi-Fi: the knob opens a captive portal AP on first boot or after a long-press reset. Only the Wi-Fi SSID and
  password are entered on a phone. Pairing is then the SignalK access request approved in the server's admin UI, no code.
- Reset setup (proposed): a long hold at power-up clears Wi-Fi and the token and reopens the portal.

## 10. MVP Scope

### 10.1 MVP Features

- Wi-Fi connect, SignalK access request and token, auto reconnect, STALE handling.
- Autopilot mode (engage/standby, target +/-1 and +/-10).
- Dimmer mode (one channel group, then several).
- Anchor mode (arm/disarm, set drop point and radius, state, ack).
- Always-on with dim mode.
- Boot to last screen under 4 s and ONLINE under 10 s (bench-measure, tighten later).

### 10.2 Post-MVP / Deferred

- USB-HID dial mode (second milestone, after Wi-Fi control works).
- Over-the-air updates and a signed firmware repository. Updates are possible over USB (MVP) and later over the air. The
  partition table, rollback and signing key stay in the build so OTA can be added without a re-partition.
- Haptics, battery, custom enclosure and mounting.
- Touch masking around knob pushes. A knob push physically presses the glass and reads as a touch near the centre
  (about 138,111 on the bench unit), about 230 ms before the button switch closes. The MVP uses a circular dead zone
  around the centre instead. Masking touches within a short window of a push (needs a small delay on touch-down) would
  let the centre be used for touch again.

## 11. References

- [Elecrow GitHub examples](https://github.com/Elecrow-RD/CrowPanel-1.28inch-HMI-ESP32-Rotary-Display-240-240-IPS-Round-Touch-Knob-Screen)
- [docs/hardware.md](docs/hardware.md)

## 12. Design Decisions

- **No companion plugin in the MVP, the knob talks to SignalK directly** (decided 2026-11-22, replaces the earlier decision
  for a plugin). One credential (the SignalK access-request token), fewer parts to install and keep in sync. The cost is that
  SignalK API details (autopilot v2, anchor alarm endpoints, alarm acknowledge) live in a firmware adapter layer. A plugin
  may return later for shims, snapshots or central config.
- **USB-HID is a selectable mode, not a separate firmware.** Both transports share the input and UI code.
- **Any autopilot through the SignalK autopilot API.** No autopilot specific code in the knob.
- **Full anchor control on the knob** (arm/disarm and drop point), alarm logic remains on the server.
- **ESP-IDF + LVGL**, for control over PCNT, boot time, OTA, power and the esp_tinyusb composite device.
- **Engage = long press + confirm turn.** Anchor changes only need a press, because they are less time critical.
- **Dimmer = `electrical.switches` dimmer paths**, with channels (name plus path) configured on the knob.
- **Build on espOS (ESP-IDF 6 components, `signalk-espos/espos_core`) with our own knob app.** espOS provides the Wi-Fi
  setup portal, NVS config, SignalK discovery/token/stream/PUT, REST/web UI, event bus and logs. It has no
  display, encoder, HID or dimming support, so those stay ours. We borrow ideas and code (Apache-2.0, 2.x line only)
  from `dirkwa/espos-p4-cockpit`: the `cockpit_hal` display/touch HAL split, `IdleDimmer` with wake overlay, `sun_state`
  and the day/night theme. We do not adopt its touch-first JSON Layout Player for the MVP: our UI is a fixed set of
  modes driven by the encoder. espOS is pre-1.0 and S3/CrowPanel support is unverified, hence the bench spike
  ([docs/spike-espos.md](docs/spike-espos.md)).
- **One credential, config on the knob** (questions 5 and 6, decided 2026-11-22). The SignalK access request is the only
  pairing step, the token stays in NVS. Layout, channels, steps, timeouts, dim levels and HID profile are set on the knob's
  own espOS settings page. The knob uses espOS's SignalK client directly for simple paths.
- **Day/night follows `environment.mode`** (`"day"` / `"night"`), as `sun_state` in the cockpit firmware does. Default to Day until the first
  value arrives, because a dark helm display is worse than a bright one.
- **Touch is secondary and ignored in the centre.** A knob push registers as a touch there (hardware coupling, not a
  bug). The dead zone is a 50 px radius around the centre, size unverified. Masking touches around pushes is deferred.
- **Plain WS and HTTP plus the SignalK token, no TLS.** Keeps connect time inside the ONLINE target on a closed boat LAN.
- **Firmware updates: USB in the MVP, OTA and a signed firmware repository later** (decided 2026-11-22, replaces the earlier
  "never pull from a server"). Nothing in the MVP points the OTA component at an update source.
- **Anchor adapter targets sbender9 `anchoralarm` first** (decided 2026-11-22). Disarm maps to `raiseAnchor`.
- **Dimmer channels carry a value form** (ratio 0..1, percent 0..100 or on/off), set on the settings page per channel.
- **Heading source is configurable** (true or magnetic, default true), shown with `°T` or `°M`.
- **Staleness age is measured from the knob's receive time**, not the server's timestamp.
- **Setup AP closes once Wi-Fi works, API key is a fixed key set by the owner on the settings page.**
- **Licence Apache-2.0.** Config export/import as JSON is planned after the MVP.
- **Configurable HID profile, default dial.**
- **Looser boot targets (4 s / 10 s)** because the device is always on, not cold-booted per use.

## 13. Open Questions

1. Night/day: `environment.mode` with `"day"`/`"night"` is what the espOS ecosystem uses. It is published by
   `signalk-espos-manager` (from sun altitude) or `signalk-derived-data`. Confirm one of them runs on the boat.
2. Dimmer paths: which `electrical.switches.*` convention does the boat actually use, and how is it PUT?
3. Anchor alarm: the espOS cockpit docs target `hoeken/hoekens-anchor-alarm` (PUT `null` on `navigation.anchor.position`
   raises, PUT `navigation.anchor.maxRadius` sets scope, dropping at the current fix needs a plugin change). Which plugin
   do we drive? Previously researched: sbender9 `anchoralarm`. Is the `anchoralarm` plugin the target? How are its alarms acknowledged, and does disarm equal `raiseAnchor`?
4. RESOLVED 2026-11-22: firmware updates over USB in the MVP, OTA and a signed firmware repository post-MVP. The two-slot table,
   rollback and signing key stay (ARCHITECTURE.md section 8). Still open for later: where the update source lives.
5. RESOLVED 2026-11-22: pairing overlap. The SignalK access request is the only pairing step, no plugin pairing code. Layout
   and channel config live on the knob's espOS settings page.
6. RESOLVED 2026-11-22: the knob subscribes and does `PUT` with espOS's SignalK client for simple paths. There is no companion
   plugin in the MVP, so autopilot and anchor calls are made by the firmware too (section 6.1, questions 8 and 9).
7. RESOLVED in the design (ARCHITECTURE.md section 2.4): the input task stamps activity itself, the dimmer does not depend on
   LVGL's inactivity counter. Still to confirm on the board (turn and push wake from dim).
8. RESOLVED 2026-11-22: espOS provides `espos_sk_http` (GET/PUT/POST with the access token, scheme from the server, 401/403 fed back
   to the token machine). Adapters use it from the actions task only, it blocks.
9. Which permission does the device token get from the access request, and is it enough for `PUT` and the autopilot v2 and
   anchor alarm calls on a server with security on?
10. Config backup and restore: can the espOS settings page export and import a knob's config, so a replaced knob or a second
    knob does not need re-entering everything?
11. Staleness: which of the boat's paths carry `meta.timeout`, and does the server enforce it (the signalk-server stale-data
    work marks timed-out paths)? How does the knob read the meta (REST `.../meta` or `meta` deltas) and compute age against the
    server's clock when the server has no RTC?
