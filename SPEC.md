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
- Allow control: autopilot and anchor commands only leave the knob while the stored per-knob setting **allow control** is on.
  It is off on a fresh knob and after a factory reset, and stays as set across reboots. It is changed on the web settings page,
  through the config API, or on the knob's settings screen (off to on needs a confirm turn). The serial command `l` flips it
  for the running session only, as a bench override. Every change is logged with its source. A refused command shows the
  refusal screen (`CONTROL IS OFF`, `NOTHING WAS CHANGED`) on the autopilot and the anchor screen alike. A knob with it off is
  a view-only knob.

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

### 6.2a The knob's REST API (#3)

On the espOS httpd, protected by the API key when one is set (optional, section 12). No path, token or address is ever in a
reply.

- **Config:** espOS's own. `GET /api/v1/config?ns=knob` reads the knob's settings, `PUT /api/v1/config` takes a partial
  document such as `{"knob":{"allow_control":true}}` (keys left out stay as they are, every key is checked against the
  descriptor, a bad one gives 400 with the key and the reason, the reply lists the changed keys and `restart_required`).
  `GET /api/v1/config` without `ns` exports the whole device without secrets, which is the backup, and `PUT` of that document
  restores it. Changes apply at once, the light channels after a restart (new paths would need new subscriptions).
- **Mode:** `GET /api/v1/knob/mode` gives `{"mode":"lights"}`. `PUT /api/v1/knob/mode` with
  `{"mode":"autopilot"|"lights"|"anchor"|"hid"}` opens that mode through the same entry point as the mode ring and stores it as
  the last mode. It cannot engage, drop, raise or acknowledge anything, and an alarm on screen stays on top. 400 for a bad body,
  409 for `hid` without a USB host.
- **Status:** `GET /api/v1/knob/status` gives `{"mode", "link", "allow_control", "demo", "alarm", "firmware"}`. `link` is
  `online`, `connecting`, `setup`, `pairing` or `failed`, `alarm` is `none`, `anchor_drag` or `gps_lost`, `allow_control` is
  what applies now (the stored setting or the serial override).

### 6.3 SignalK interfaces the knob uses

Researched against the SignalK server docs and the anchor alarm plugin README. The firmware calls these through its adapter
layer (section 6.1).

- **Autopilot (v2 API)**, base `/signalk/v2/api/vessels/self/autopilots/{id}` (`_default` for the default device):
  `GET` status (`state`, `mode`, `target` in radians, `engaged`), `POST engage`, `POST disengage`,
  `PUT state`, `PUT mode`, `PUT target` and `PUT target/adjust` with `{"value": -2, "units": "deg"}`.
  Deltas arrive on `steering.autopilot.*`. An unreachable device reports `off-line`.
- **Anchor alarm** (`hoekens-anchor-alarm`, the plugin installed on the boat, decided 2026-11-23). REST under
  `/plugins/hoekens-anchor-alarm/`, needs a `readwrite` token (the knob's token qualifies): `POST dropAnchor`
  `{position:{latitude,longitude}, zone:{type:"circle", radius}}` (arm, the boat must be inside the zone), `POST setZone`
  `{zone, position?}` (change the radius or move the drop point and keep the session), `POST raiseAnchor` (disarm, no body).
  Replies `{statusCode, state}`, errors come as 403 with a message. The knob has no position of its own: it uses the latest
  `navigation.position` it subscribed to as the drop position. It publishes `navigation.anchor.state` (`"on"`/`"off"`),
  `.position`, `.currentRadius` (m, antenna to anchor), `.maxRadius` (m, circle only), `.watchZone`, `.distanceFromBow`,
  `.bearingTrue` (rad) and the alarm on `notifications.navigation.anchor`. The knob only sets circle zones.
- **Music (signalk-jukebox, #6)**: `entertainment.jukebox.playback.volume` (ratio, `PUT`), `.playback.state`,
  `.playback.track` (object `{name, artist?, album?}`), `.zones.<id>.volume` (ratio, `PUT`), `.zones.<id>.connected`, and the
  momentary controls `.playback.controls.play|pause|next|previous` (`PUT` 1 then 0). Zone names from
  `GET /plugins/signalk-jukebox/api/zones` with the token. Allow control is not needed for these.
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

NVS: pairing/token, server host, last mode (the last mode chosen on the ring, and whether USB-HID was chosen after it, see
docs/ui-design.md section 3.4), brightness limits, HID/Wi-Fi mode selection.
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
- Boot to the last screen and ONLINE under 10 s. The splash shows the connection phases until the token is verified, then
  the last screen opens (decided 2026-11-23, the earlier 4 s target for the last screen was dropped).

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
  bug). The dead zone is a 50 px radius around the centre, size unverified. Masking touches around pushes is deferred. A
  touch anywhere, the dead zone included, wakes a dark screen and is swallowed (decided 2026-11-23, docs/ui-design.md
  section 6).
- **Plain WS and HTTP plus the SignalK token, no TLS.** Keeps connect time inside the ONLINE target on a closed boat LAN.
- **Firmware updates: USB in the MVP, OTA and a signed firmware repository later** (decided 2026-11-22, replaces the earlier
  "never pull from a server"). Nothing in the MVP points the OTA component at an update source.
- **Anchor adapter targets `hoekens-anchor-alarm`** (decided 2026-11-23, replaces the earlier choice of sbender9
  `anchoralarm`, which is not installed on the boat). Arm is `dropAnchor`, radius or drop point change is `setZone`, disarm is
  `raiseAnchor`. The knob sets circle zones only.
- **Dimmer channels carry a value form** (ratio 0..1, percent 0..100 or on/off), set on the settings page per channel.
- **The dimmer mode is called LIGHTS and handles on/off channels** (decided 2026-11-23). The boat has no dimmers, only relays
  (`electrical.switches.bank.0.N.state`, values 0 and 1). On an on/off channel turn right is on and turn left is off. Level
  dimmers stay supported for other boats. Each channel's form (on/off or level) is part of the channel config.
- **Heading source is configurable** (true or magnetic, default true), shown with `°T` or `°M`.
- **Staleness age is measured from the knob's receive time**, not the server's timestamp.
- **Setup AP closes once Wi-Fi works. The API key is optional** (decided 2026-11-23, replaces "a fixed key set by the owner"):
  a boat's instrument Wi-Fi counts as a trusted network, so the settings page and the REST API are open by default. An owner who
  wants a key sets `httpd.api_key` on the settings page, then every call needs it.
- **Licence Apache-2.0.** Config export/import as JSON is planned after the MVP.
- **Configurable HID profile, default dial.**
- **Looser boot targets (10 s for the last screen and ONLINE)** because the device is always on, not cold-booted per use. The
  splash with its phases until the token is verified reads better than a mode screen under an overlay (2026-11-23).
- **Config API is espOS's own** (decided 2026-11-23, #3). `GET` and `PUT /api/v1/config?ns=knob` (with the API key when one is set), validated
  against the descriptor, partial updates, `restart_required` in the reply. The whole-device export (secrets left out) is the
  backup and restore format. The knob adds live reload, `GET`/`PUT /api/v1/knob/mode` and a read-only `GET /api/v1/knob/status`.
- **Allow control is a stored per-knob setting** (decided 2026-11-23, #3 and #5, replaces the bench latch "live control", which
  was off at every boot and only switched by the serial command `l`). Config key `knob.allow_control`, default off. It can be
  switched on and off through the config API (with the API key when one is set). When it goes on from the web page or the API, the knob shows
  a short `CONTROL ON` notice. There is no permanent indicator: the refusal screen says when it is off.
- **Demo mode** (decided 2026-11-23, #4): simulated data, no SignalK client, Wi-Fi and the settings page stay up. Stored in the
  config (survives a reboot), off by default, cleared by a factory reset.
- **VOLUME mode drives `signalk-jukebox`** (decided 2026-11-23, #6). Master volume and zone volumes through SignalK `PUT`
  (ratio), play and pause through the plugin's control paths, zone names from its REST API. Knob: turn 2 %, press play or
  pause, hold next zone. The touch screen is a small player (previous, next, play or pause, zone), the outer arc is display
  only. No allow control needed.
- **Autopilot wind and route modes, tack from the knob** (decided 2026-11-23, #7). Modes are the provider's states, changed
  from a long-press dialog (`STANDBY` first and focused, then the other modes, tack port and starboard, cancel). Tack needs a
  confirm turn. Engage always sends `PUT state auto`, never `POST engage`, because the provider's engage resumes the last
  state. Wind target changes wait for an upstream fix to `signalk-autopilot`, until then wind mode is display only.
- **The manual is `docs/manual.md`** (decided 2026-11-23, #2), linked from the README and the settings page.

## 13. Open Questions

1. Night/day: `environment.mode` with `"day"`/`"night"` is what the espOS ecosystem uses. It is published by
   `signalk-espos-manager` (from sun altitude) or `signalk-derived-data`. Confirm one of them runs on the boat.
2. Dimmer paths: which `electrical.switches.*` convention does the boat actually use, and how is it PUT?
3. RESOLVED 2026-11-23 for the target plugin: the boat runs `hoekens-anchor-alarm` (see section 6.3 and the decisions). Still to
   verify on the boat: that the alarm is acknowledged through the Notifications API. The older note: the espOS cockpit docs target `hoeken/hoekens-anchor-alarm` (PUT `null` on `navigation.anchor.position`
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
9. RESOLVED 2026-11-23: the token gets what the admin approves, one of `readonly`, `readwrite` and `admin` (the only levels
   signalk-server 2.37 knows). Every write, `PUT` and v2 API call checks for `readwrite` or `admin`, so `readwrite` is the
   narrowest token that works. Finer limits are the server's ACLs (read, write or put per path and subject), set up on the
   server if wanted, nothing on the knob. Still open: whether the autopilot and anchor calls pass with it (live control trials).
10. RESOLVED 2026-11-23: config backup and restore is espOS's `GET` and `PUT /api/v1/config` (whole device, secrets left out,
    versioned per namespace). See section 12 and docs/plans/issue-3-rest-api.md.
11. Staleness: which of the boat's paths carry `meta.timeout`, and does the server enforce it (the signalk-server stale-data
    work marks timed-out paths)? How does the knob read the meta (REST `.../meta` or `meta` deltas) and compute age against the
    server's clock when the server has no RTC?

### Boat survey, 2026-11-23

Read-only survey of the boat's SignalK server (2.37.0, unauthenticated reads allowed) plus one authenticated test write
through the knob. Answers for questions 1, 2, 3, 9 and 11:

- **Day and night (1):** `environment.mode` is published by `derived-data`, string `"day"` or `"night"`, event driven (its
  timestamp can be hours old). `environment.sun` also exists with the same words. Use `environment.mode`.
- **Dimmers (2):** the boat has **no dimmers and no `dimmingLevel`** anywhere. The switchable loads are relays. The 8-relay
  board (Waveshare ESP32-S3-ETH-8DI-8RO, running espOS) shows up as `electrical.switches.bank.0.N.state`
  (N 1 to 8, displayName "Relay N", values `0` and `1`, also via NMEA 2000 PGN 127501) and a second bank `bank.1.N`. A `PUT` of
  `1` or `0` to `electrical.switches.bank.0.1.state` through the knob's token **works** (completed with 200, the value followed
  on the server). So the knob's dimmer mode needs the on/off value form first. Level dimming stays supported in the UI for boats
  that have it.
- **Token permission (9):** the knob's access request was approved with `readwrite`, and a `PUT` succeeded. Autopilot and anchor
  calls with that token are not tested yet.
- **Autopilot:** the v2 API lists one provider, id `raymarineST` (not `_default`), states `standby`, `auto`, `wind`, `route`.
  Currently `state` and `target` are `null` (the pilot is not active) and `.../mode` answers "Not implemented", so engage and
  disengage go through `state` and `engage`. There are **no `steering.*` paths** (no rudder angle, no `steering.autopilot.*`).
  Heading: `navigation.headingTrue` (derived-data) and `navigation.headingMagnetic` (NMEA 2000), both in radians.
- **Anchor (3):** no `navigation.anchor.*` paths are present right now because no watch is active. The boat runs
  `hoekens-anchor-alarm` (confirmed by the owner, sbender9 `anchoralarm` is not installed). The v2 Notifications API exists
  and supports acknowledge and silence. Whether the plugin's drag alarm is acknowledged through it is still to verify with a
  real alarm.
- **`meta.timeout` (11):** none of the paths the knob needs (headings, switches, mode, GNSS) carries `meta.timeout`. Only the
  espOS device health paths do (25 s). So the per-path defaults in ARCHITECTURE.md section 3.1 are what applies.
- **Pilot wiring (added later on 2026-11-23):** the pilot is a Raymarine SeaTalk 1 unit and is not wired to the server yet. The
  `raymarineST` provider emits its datagrams on `seatalkOut`, which no data connection consumes, and no NMEA 0183 output
  sends a route to the pilot. Autopilot commands therefore reach no pilot until that is set up (docs/plans/issue-7-autopilot-modes.md).
- **Not on this boat:** no `environment.depth.*`, no steering paths. The depth and rudder telemetry on the autopilot screen
  will show `---` unless a source is added.
