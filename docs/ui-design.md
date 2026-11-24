# UI design: signalk-rotary-knob

Status: agreed in a design conversation, DRAFT. Builds on SPEC.md sections 2, 3, 7 and 12. Items marked **(verify)** depend on
SignalK paths or hardware behaviour that is not confirmed yet.

This design started from an external UI proposal (a touch-first prototype with LVGL v8 code). Its visual language is kept, its
interaction model is replaced.

## 1. Principles

- Encoder first. Every function works from the knob alone. Touch is secondary.
- The knob shows and requests, the server decides. The UI never mutates autopilot or anchor state locally. It sends an action
  and shows the state that comes back in the next snapshot.
- Stale data is never shown as current.
- One glanceable home per mode, no menu diving at the helm.

## 2. Input vocabulary

| Input | Meaning |
|---|---|
| Turn | Per mode, one click is one step |
| Single press | Per mode, accepted after about 300 ms because a second press may follow |
| Double press | Open the mode ring (all modes) |
| Long press | Per mode, state-changing or safety-relevant actions |
| Touch | Non-critical actions only (for example swipe between modes, dimmer level). Never autopilot or anchor changes. Ignored within 50 px of the centre because a knob push reads as a touch there. Ghost touches are filtered in the driver |

The 300 ms single-press delay means single presses are used only for non-critical actions (step size, next channel, apply a
previewed value, acknowledge is the exception, see section 5).

### Mode ring

Double press opens a ring overlay listing the modes enabled in the knob's config (Autopilot, Dimmer, Anchor, optional HID). Turn to
move, press to select, double press or a few seconds without input cancels. The ring is drawn as the proposal's radial
switcher, scaled to the number of visible modes (and the position text, for example "2 of 3", counts visible modes only).

The HID mode is listed only while a USB host has enumerated the knob (section 3.4). Without a host it is hidden from the ring.

MOTOR sits after AUTO when `knob.motor_mode` is on (section 3.7, off by default). MUSIC sits after ANCHOR when
`knob.music_mode` is on (section 3.6). The full order: AUTO, MOTOR, LIGHTS, ANCHOR, MUSIC, USB, SETUP. The last slot is **Settings** (gear icon, label `SETUP` because `SETTINGS` does not fit the 54 px slot, section 3.5), hidden
while an alarm is active. Agreed 2026-11-23 from a mockup: the visible slots are spaced evenly at r 86, clockwise from the top
(5 with a USB host, 4 without). The centre shows the mode you are in.

## 3. Modes

### 3.1 Autopilot

Shows: heading (large), target (CTS bug on the perimeter ring), delta to target (computed on the knob), autopilot mode, engaged
or standby, rudder angle bar, depth, voltage and GPS fix. Rudder, depth, voltage and GPS fix need SignalK paths set in the
knob's config **(verify)**.

| State | Turn | Single press | Long press |
|---|---|---|---|
| Standby | Nothing (or scroll the mode, see open items) | Nothing | Start engage: prompt, then confirm turn |
| Engaged | Change target by the current step (1 or 10 degrees) | Toggle step 1 / 10, shown on screen | Open the pilot dialog with `STANDBY` first and focused, so long press and one press is standby (decided 2026-11-23 with #7, replaces the direct standby). The dialog also lists the other pilot modes and tack, see docs/plans/issue-7-autopilot-modes.md |

- Engage is a long press followed by a confirm turn, as in SPEC. The confirm turn direction and timeout (suggested: one click
  clockwise within 5 s) are to be fixed when building.
- Target changes are sent after a short pause (suggested 400 ms) so a run of clicks is one request. The displayed target is the
  server's value, with the pending value drawn as a preview until it is confirmed.
- No local "sync CTS to heading on engage". The server's autopilot API decides the target.
- With allow control off (SPEC section 2) an engage, standby or target change is not sent. The screen shows the same refusal
  as the anchor (section 3.2): red cross, `CONTROL IS OFF`, `NOTHING WAS CHANGED`, a press dismisses it. Decided 2026-11-23,
  replaces dropping the command silently.

#### Wind and route (agreed 2026-11-23 from mockups, #7)

The pilot's other states, chosen from the pilot dialog while engaged (docs/plans/issue-7-autopilot-modes.md). Engage from
standby always engages compass.

- **Pilot dialog** (long press while engaged): top line `PILOT · <current mode>`, lines `STANDBY` (first, focused, amber pill),
  the other modes the pilot lists (`COMPASS`, `WIND`, `ROUTE`), `TACK PORT` and `TACK STARBOARD` (compass and wind only),
  `CANCEL` muted. Turn chooses, one press confirms. A tack line then asks for a confirm turn towards the tack side within 5 s.
- **Wind:** top line `WIND · AWA`, apparent wind angle large with `P` or `S` (`118° P`), `TGT 120° P · 2° off` in amber under it,
  heading small below. On the rim: the red lubber mark, a cyan line from the centre for the AWA and an amber dot for the target.
  Turn adjusts the target by the step once the provider supports it, until then the bottom line reads `ADJUST ON PILOT` and the
  turn does nothing.
- **Route:** top line `ROUTE · TO <waypoint name>`, bearing and distance to it, XTE large (`0.08 nm`) with `steer right` or
  `steer left`, a steer bar like the rudder bar (fill from the centre towards the side to steer), heading and `HOLD: PILOT` on
  the bottom line. Turn does nothing.
- Built 2026-11-23 as variants of the compass screen's labels: the header line says `WIND · AWA` or `ROUTE · TO <waypoint>`,
  the large number is the AWA or the XTE, the pill says the mode, the route's steer bar is the rudder bar. The waypoint shows as
  `WAYPOINT` until a name source is found. Engaged compass says `COMPASS · STEP n°` in the pill.
- With the pilot's data stale the dialog still opens with a long press, but only offers `STANDBY` and `CANCEL`. The dialog
  closes after 5 s without input, on a long or double press, or when the pilot goes to standby by itself.
- The engage prompt reads `TURN RIGHT TO ENGAGE COMPASS`, the tack prompt `TURN LEFT TO TACK PORT` or `TURN RIGHT TO TACK
  STARBOARD`, both with the 5 s countdown arc.
- LEDs: the steering indicator follows the error of the mode: compass the target minus the heading, wind the AWA against its
  target, route the side to steer from the XTE (10 m counts as on track).

### 3.2 Anchor

The watch circle is the plot (design B, agreed 2026-11-23). Words: the anchor is **dropped** or **raised**, the autopilot is
engaged or in standby, the two are never mixed.

Screens (all text flat; the state line on top (20 px, y 26) and the telemetry line at the bottom (14 px, y 210) are straight labels,
changed from curved 12 px on 2026-11-23 at the owner's request, section 8):
- **Dropped:** cyan ring (the chosen radius, 72 px), a faint ring at 50 %, the anchor in the centre, the green boat dot at its
  bearing and distance, a short trail of earlier positions, the distance next to the dot, the radius under the top of the ring.
  Top line `DROPPED`.
- **Raised:** grey ring, the boat dot in the centre (the drop point), `HOLD TO DROP` and `TURN TO SET RADIUS`. Top line `RAISED`.
- **Radius preview** (while turning): the old ring fades back, the new radius is an amber ring, large amber `40 m`,
  `PRESS TO APPLY`. Top line `SET RADIUS`.
- **Stale:** the plot is hidden, the stale overlay says why (section 6).
- The bottom line shows only what the boat publishes (depth, volts, GPS fix). A value that never arrived is left out, a stale one
  shows `---`.

Controls:
- Turn: preview a new radius (steps of 5 m, range from the knob's config, proposed 10 to 150 m).
- Single press: apply the previewed radius. While raised it is only remembered for the next drop, while dropped it goes to the
  server.
- Long press: open the dialog, which lists only the valid actions and always ends with **Cancel**. Raised: **Drop here**
  (with the radius, at the current position). Dropped: **Move drop point here**, **Raise anchor** (red), Cancel. Turn chooses,
  one press confirms, a long press or 5 s without input leaves it. A radius turned to but not applied is the one used to drop.
- After a choice the screen shows what is happening: an amber spinner (`DROPPING...`, `MOVING DROP POINT...`, `RAISING...`,
  `SETTING RADIUS...`) until the server confirms through the data stream, or a red cross with the reason and `NOTHING WAS CHANGED`
  (`CONTROL IS OFF`, `NO LINK TO THE SERVER`, `NO POSITION FIX`, `THE SERVER REFUSED IT`, `NOT CONFIRMED` after 8 s). A press
  dismisses it. The firmware reports the result in the snapshot (`action_result`, `action_result_seq`).
- Alarm acknowledge: see section 5.

All anchor changes need only the one confirm press (SPEC section 2). Alarm logic stays on the server.

### 3.3 Lights (dimmer)

The mode is called **LIGHTS** (ring label and screen), decided 2026-11-23, because the boat has relays and no dimmers. A channel
is either a dimmer (level) or an on/off channel (a relay). On an on/off channel: turn right switches it on, turn left switches it
off, one click is enough. The screen shows a large `ON` or `OFF` (no percentage), the arc is full when on. Single press picks the
next channel and long press toggles, as for dimmers. The first-click rule below applies to dimmers only.

- Turn: level of the shown channel group in 5 % steps.
- Single press: next channel group, in the order set in the knob's config.
- Long press: toggle the group off or on (restores the previous level).
- Turning an off group that remembers its level: the first click only switches the light back on at that level and does not
  change it. Further clicks are ignored until the server shows the group on (or 3 s pass), then turning changes the level
  as usual. An off group with no remembered level (0 %) takes the turn as a normal level change.
- The ring shows the level, a row of dots shows which group is selected. Touch may also set the level.
- Look: tick ring in light grey, an amber level arc around the ring, large level numeral in the centre, the group name in a
  pill, a dot row with "GROUP n OF m", the hint line along the bottom (`TURN: LEVEL · PRESS: NEXT`).
- Group off: the level arc and numeral are replaced by a grey `OFF`, the screen shows the last level and the hint
  `HOLD TO TURN ON`.

### 3.4 USB-HID

USB-HID is a transport, selectable as a mode. Wi-Fi stays connected in the background.

- Turn: send the configured HID action (for example zoom).
- Single press: send the click or enter.
- Long press: cycle the HID profile (chartplotter, media, cursor keys). The profile is also set in the knob's config.
- Alarms still take over the screen (section 5).

Availability:

- HID is listed in the mode ring only while a USB host has enumerated the knob (TinyUSB mounted). Plain power from a charger
  does not count. With no host, HID is hidden.
- If the host disappears while the knob is in HID mode, it **stays on the HID screen** with "USB DISCONNECTED" and
  "NO USB HOST". HID is already hidden from the ring, so the user leaves through the double-press ring as usual. The SignalK
  link stays up and alarms still take over the screen. It does not switch mode by itself, and it does not resume on its own
  when the host returns, the user selects HID again from the ring (open item 10).
- Boot: the knob boots into HID (when HID was the last mode) only if a USB host is on the cable within the boot window, the
  first 10 s after power on, and the mode has not been changed meanwhile. Otherwise it boots to the last non-HID mode.

Look: tick ring, no gauge arc. A green `USB HID CONNECTED` label sits at the top, the profile name is in the centre, the
live action line shows the last action (`ZOOM IN (+)`) and `PRESS: ENTER`, and `HOLD: PROFILE` at the bottom. Without
a host, everything greys out and the labels read `USB DISCONNECTED`, `NO USB HOST`, `SIGNALK LINK STILL UP` and
`2x PRESS: MODES`. A later option is to show a configured SignalK path on the HID screen (open item 11).

### 3.5 Settings

Agreed 2026-11-23 from a mockup (#5, docs/plans/issue-5-settings-screen.md). Opened from the last slot of the mode ring, left
with a double press. Only what a skipper changes at the helm, everything else stays on the web settings page.

Look:
- **List:** the focused item in a pill in the middle (name, value in the primary colour), the item before and after it in muted
  text above and below, `SETTINGS 3/14` on the top line, `PRESS EDIT · 2x BACK` on the bottom line (straight, 12 px).
- **Editing:** the item name on the top line, the new value large in amber on a 270 degree amber arc, `was 80 %` under it,
  `PRESS STORE · 2x UNDO` on the bottom line. On/off and choice items show the word instead of the arc.
- **MODES page** (built 2026-11-24, owner's request): six pills in two columns and three rows, AUTO, LIGHTS, ANCHOR,
  MUSIC, MOTOR, USB. A filled pill (primary colour, dark text) is a mode on the ring, an outlined one is off. Turn moves
  the focus (a thicker border), a press switches the focused mode, a double press goes back to the list. The last mode that
  is on stays on: the bottom line says ONE MODE STAYS ON for 2 s. Settings is always on the ring. The list shows
  5 OF 6 ON. Stored as knob.ap_mode, lights_mode, nchor_mode, music_mode, motor_mode and usb_mode (also on the
  web settings page). A mode switched off while it is on screen hands over to the first one that is on, and so does a boot
  into it. USB still needs a USB host on the cable to show.
- **Confirm turn** (allow control, off to on): cyan countdown arc, `Autopilot and anchor`, `TURN RIGHT`, `to allow`,
  `5 s · ANY PRESS CANCELS`.
- Night: the same layout in the red palette.

| Input | List | Editing |
|---|---|---|
| Turn | Move | Change the value |
| Single press | Edit (or open a read-only page) | Store |
| Double press | Leave settings | Cancel the edit |
| Long press | Not used | Not used |

Items, in order: active brightness, dim after, dim level, off after, night mode (server, day, night), LEDs (on or off, all
modes), heading reference (true or magnetic), HID profile, allow control, demo mode, setup access point, device info,
SignalK, reset setup (14 items).

Steps a turn moves through (clamped at the ends): brightness 10 to 100 % in tens; dim after 5, 10, 15, 20, 30, 45 s, 1, 1.5, 2,
5, 10, 30, 60 min; dim level 1, 2, 5, 10, 15, 20, 30, 40, 50, 60, 80, 100 %; off after never, 1, 2, 5, 10, 15, 30, 60, 120, 240
min. Values from the web page that sit between two steps move to the nearer step in the turn's direction.

- **Allow control:** off to on is a press and then one click right within 5 s. On to off is one press.
- **LEDs off** turns off every LED in every mode, the alarm flash included: an alarm then shows on the screen only.
- **Setup access point:** a press opens the espOS setup AP next to the current Wi-Fi so a phone reaches the settings page,
  nothing is cleared. The value shows `OPEN 9:59` counting down. It closes after 10 minutes or with another press on the item.
  It stays shut if the AP is disabled on the web page.
- **Device info** and **SignalK** are read-only pages: firmware, Wi-Fi network and signal, IP, free RAM, uptime; server, link
  state, token state, allow control.
- **Demo mode:** storing a change restarts the knob into or out of demo mode (#4).
- **Reset setup:** a press opens a page with a red arc that fills while the button is held, `HOLD 5 s`, `CLEARS WI-FI`,
  `AND PAIRING`. Released early nothing happens, a short press goes back. After 5 s the knob forgets the Wi-Fi networks and the
  SignalK token and restarts into the setup access point.
- The settings work without a SignalK link (no stale overlay), so the setup access point and the info pages are always
  reachable.
- Nothing here changes autopilot or anchor state or acknowledges an alarm. An alarm takes over the screen as everywhere.

### 3.6 Music (volume)

Agreed 2026-11-23 from a mockup (#6, docs/plans/issue-6-volume-mode.md). Drives the `signalk-jukebox` plugin. Allow control is
not needed.

| Input | Meaning |
|---|---|
| Turn | Volume of the selected target, 2 % per click, sent after a 0.4 s pause |
| Single press | Play or pause |
| Long press | Next target: Master, then each zone |
| Touch | Previous (left), next (right), play or pause (bottom), zone name (top, next target). Nothing in the 50 px centre, nothing on the outer arc |

Look: the outer arc (270 degrees, cyan) shows the volume and nothing else. Inside: the target name at the top (`MASTER` or the
zone name, with a small marker that it can be tapped), track name and artist under it, the volume number large in the centre,
previous and next as round buttons left and right, play or pause as a round button at the bottom. A pending volume is drawn in
amber until the server confirms it. Paused greys the arc. A zone that is not connected shows `ZONE OFFLINE`. A stale volume
shows the stale overlay (`NO VOLUME`).

On the mode ring MUSIC (music notes icon) sits after ANCHOR: AUTO, LIGHTS, ANCHOR, MUSIC, USB, SETUP. The setting
`knob.music_mode` (on by default) takes it off the ring on a boat without the jukebox. Up to four zones, named from the
plugin's REST API (the id until the name arrives). The master volume never received counts as `NO VOLUME`, like a stale one.

### 3.7 Motor

Decided 2026-11-24 with the owner (#8, docs/plans/issue-8-motor-mode.md). The rules are knob_motor in knob_logic; the screen only
shows its state. Needs allow control. Not verified on a real motor yet.

| Input | Disarmed | Armed |
|---|---|---|
| Long press | Arm: then one click right within 5 s (countdown arc). Refused out of neutral, without allow control, without a fresh gear, on a fault | Out of neutral: throttle 0 at once (at 750 ms, while still holding), stays armed. In neutral: disarm |
| Turn | Nothing | Right more ahead, left more astern. 1 % per slow click (over 150 ms apart), more when faster (gentle 1/1/2, normal 1/2/5, quick 1/3/10 %). Stops at neutral from either side; still for 1 s, then on into the other direction. Clicks in the first 500 ms after arming are ignored |
| Single press | Nothing | Nothing (it only counts after the 300 ms double-press wait, too slow for neutral) |
| Double press | Mode ring | Limit warning showing: lift both limits until the next disarm, the throttle stays. Otherwise the mode ring (the throttle keeps its value) |

Look: neutral at 12 o'clock. Ahead is a green arc clockwise, astern an amber arc counter-clockwise, 150 degrees each way for
100 %; a white dot shows what the motor reports, small amber dots mark the limits until they are lifted. The centre: the gear the
motor reports, large (`F` green, `N`, `R` amber, `!` red for a fault, `---` without a gear), the set throttle (`AHEAD 40 %`),
then RPM, and torque and power when the boat publishes them. Top line: `MOTOR`, `ARM MOTOR?`, `ARMED`, or for 5 s why the knob
refused or let go (`CONTROL IS OFF`, `NO LINK`, `NO MOTOR DATA`, `SET NEUTRAL FIRST`, `IDLE, DISARMED`); `MOTOR FAULT` in red
stays until the next arm. Bottom line: `HOLD: ARM`, `TURN RIGHT TO ARM`, `HOLD: NEUTRAL`, `HOLD: DISARM`, or the limit warning
`LIMIT 50 % · 2x: ALLOW MORE` in amber.

- Limits: max ahead and max astern, 50 % by default. The first time in an arm cycle the throttle stops there and the warning
  shows, nothing above the limit is sent.
- The knob falls back to neutral (sends 0, disarms) on allow control off, link loss, a gear older than 3 s, a `Fault`, a gear
  that does not match the throttle's sign for 3 s, or 60 s in neutral without input (0 = never). A reboot starts disarmed and
  sends nothing.
- While out of neutral: the throttle is resent every 1 s (the motor side must go to neutral after about 3 s without it), every
  other screen except the alarm shows `F 40 %` (green) or `R 15 %` (amber) above the DEMO tag, and the screen neither dims
  nor goes off.
- LEDs (section 7): ahead fills 1 o'clock then 3:30 in green, astern 11 then 8:30 in amber (`knob_led_fill`, two per side), the
  bottom LED the reported gear (green F, amber R). Dark at night. The limit warning flashes all five amber at 4 Hz on every
  screen, also at night.
- The motor's inputs skip the stale gate: knob_motor refuses or falls back by itself, and a long press for neutral is never
  swallowed. A stale link still shows the stale overlay.

## 4. Link state and stale handling

States follow SPEC section 3.1. In STALE (and while connecting):

- The mode screen stays underneath a dark overlay with a clear message (for example "Link lost") and the age of the last data.
- The values under the overlay are masked or blanked so an old heading cannot be read as live.
- All controls of the stale mode are ignored. The mode ring still opens, so a stale sensor never traps the user in a mode. HID
  mode keeps working without a SignalK link, it only needs the USB host.
- The LEDs show a slow amber pulse.
- The knob never sends an action while stale. Losing the knob changes nothing on the boat (SPEC section 2).

### Demo mode (#4)

The whole UI runs on a simulated boat. A small `DEMO` (12 px, purple, red at night) sits at the bottom edge, below the bottom
lines, on every screen. That tag is the only sign of demo mode: the LEDs show what they show on the boat (decided
2026-11-24, the purple LED pulse was dropped). Everything else
looks and works as on the boat, alarms included: setting the anchor radius below the boat's swing raises the drag alarm.

### Automatic demo (#10, built 2026-11-24)

`DEMO MODE` has three values: `OFF`, `ON` (the manual demo above) and `AUTO`. In `AUTO` the knob drives itself through every
mode the way a person would, always through the mode ring, and runs one typical action in each (the script is
`knob_ad_script` in `components/knob_logic/knob_autodemo.c`, docs/plans/issue-10-auto-demo.md). It starts with a 3, 2, 1
countdown, then the splash with the boot phases (`WI-FI`, `SIGNALK`, `TOKEN`), then the tour, about two and a half minutes,
and starts again from the top.

The simulated inputs go through the same entry points as the hardware, so the real gesture timing applies, and every one is
drawn on the top layer, above every screen and the alarm takeover:
- **Turn:** a short arc on the rim on the turning side (right for clockwise), a chevron at its end and the click count
  (`+3`, `-12`), gone 0.6 s after the last click.
- **Press:** a ring grows from the centre and fades. **Double press:** two of them. **Long press:** a ring around the
  centre fills for the 750 ms hold, then the press ring.
- **Touch:** a fingertip dot at the point with a ripple.
- **Caption:** one short line in a pill at the top edge (`ENGAGE`, `23° TO PORT`, `ACKNOWLEDGE`).
- **Countdown:** a large digit on a dark screen.

All in the primary colour, so red at night. In the automatic demo the lights are three simulated channels (two dimmers and a
switch), a USB host counts as connected, the HID profile and the mode stay in memory, nothing is written to flash, and no
USB device is started. The last step stores `OFF` in the settings: shown, not applied, and the tour starts again.

Any real input pauses the tour and hands the knob to the person. After 60 s without input the tour starts again from the
top. The serial command `p` starts or stops it, and outside demo mode switches demo mode to `AUTO` (the knob restarts into it).

### CONTROL ON notice (agreed 2026-11-23, #3)

When allow control goes on from the web page or the API (never when it is switched on at the knob), the current screen shows a
pill 160 x 44 at y 152, radius 22, card fill, 2 px green border, `CONTROL ON` (14 px, green) over `SET OVER THE NETWORK` (12 px,
muted), and a 3 px green rim at r 118. It stays 3 s and wakes the backlight. A press only dismisses it, turns work as usual.
An alarm screen hides it and it is not shown again. In the night palette green becomes `#FF1E27`. There is no permanent
indicator and no notice when control goes off: the refusal screen says it then.

### Per-value staleness

Staleness is decided per value, not by the stream alone. This follows the SignalK metadata, which has an optional `timeout`
for a key: how many seconds a value stays valid, possibly fractional (SignalK 1.5.0 metadata page). The older 1.0.4 text
phrases it as how long a consumer waits for a new value, so we use the 1.5.0 meaning.

- The knob keeps, for every subscribed value, its age and its timeout.
- Timeout source, in order: the key's `meta.timeout` from the server (read after connect, refreshed on meta updates), else the
  per-path default from the knob's settings. `timeout: 0` (or an event-driven key such as `navigation.state`) means the value
  never times out by age, only by link loss. Proposed defaults, to tune on the boat: autopilot state, heading, target and rudder
  5 s, anchor distance and position 10 s, depth 10 s, voltage 30 s, GPS fix 10 s. Dimmer levels and `environment.mode` have no age
  timeout.
- If the server marks a path as timed out (the stale-data RFC proposes `value: null` with a `timedOut` state, not confirmed
  shipped), the knob treats it as stale at once **(verify)**.
- Age is measured against the server's clock, not the knob's. On subscribe the server sends cached values with their old
  timestamps, so age is the delta timestamp compared with the newest server timestamp seen, advanced by the time since it arrived.
  Servers without an RTC can have a wrong absolute time, so we never compare to the knob's own wall clock **(verify)**.
- One value stale: only that value is masked (`---` in the muted colour) and any control that needs it is ignored. Other values and modes
  are unaffected. Rudder, depth, voltage and fix are display-only, so they only mask.
- Essential values per mode: autopilot state, heading and target. Anchor distance and radius. The dimmer levels. If an essential
  value is stale, the mode shows the stale overlay (section 4 above) with the reason, for example `NO HEADING`, and ignores that mode's
  controls (the mode ring still works). HID has none.
- Stream down or no link: the full stale overlay as before (`LINK LOST`, last data age). While no data has arrived since boot
  the same overlay says `CONNECTING` with the boot phase (`WI-FI`, `SIGNALK`, `TOKEN`) instead of an age, because nothing is
  lost yet.

### Boot, setup and pairing screens

Same style as the mode screens (dimmed tick ring, bold type), the status on top straight in 20 px, the line at the
bottom straight in 12 px (section 8), hidden on the splash, which shows its phase in its own status pill. Implemented in `ui/marine_ui_v5` as the system
screens.

| Screen | Shows |
|---|---|
| Boot | The splash picture (assets/splash.png) with a live progress bar and a status bar under it that names the current phase in text: `STARTING`, `WI-FI`, `SIGNALK`, `TOKEN`. It stays until the token is verified (about 6 s on the boat), then the last mode opens with a short `CONNECTING` overlay until the data arrives. Later drops never bring the splash back, they show `LINK LOST` over the mode. The last mode is the one last chosen on the mode ring (USB-HID only with a host, see section 3.4). The portal, approval and cannot-connect screens below replace it when they apply. No input needed |
| Wi-Fi setup portal | `JOIN WI-FI`, the setup network name in a pill (an example, the real name comes from espOS), `THEN OPEN THE SETUP PAGE`, `SETUP ONLY` at the bottom. Wi-Fi only, no pairing code |
| Connecting | Three steps with drawn status marks: Wi-Fi, server, link (done = green disc with a check, current = cyan ring, pending = grey ring) |
| Waiting for approval | Large straight text in the middle of the screen (28 px, three lines): `PAIRING`, `WAITING FOR`, `APPROVAL`. No status lines. Under it, small: `APPROVE IN SIGNALK ADMIN UI` and the device name the request shows there. First connect only |
| Cannot connect | `NO SERVER`, `WI-FI OK`, retrying, controls off. This is the state before the first link, not the stale overlay |
| Reset setup | Red progress arc that fills over the full 5 s hold, release to cancel, `CLEARS WI-FI AND PAIRING`. The hold is made at power-up (proposed) |

Before the link is up the knob shows these instead of a mode screen. A night palette variant uses the same layout in the red
palette.

## 5. Alarm behaviour

- An active anchor alarm takes over the full screen, whatever the current mode.
- Backlight wakes to full brightness and all five LEDs flash red (highest LED priority).
- A single press acknowledges, even on a dimmed screen, then the UI returns to the previous mode. The acknowledge is only a
  request. The server decides when the alarm is cleared. The acknowledge endpoint is unconfirmed **(verify)**, see SPEC
  section 13 question 3.
- After the acknowledge, the full-screen takeover goes away and the UI is back in the previous mode with every control
  usable again (autopilot, dimmer, mode ring). While the server still reports the alarm, a **persistent red ring** stays
  around the screen edge (pulsing slowly, over the tick ring) with a small red label (for example `DRAG ALARM`) near the top, and
  the LEDs keep a slow red pulse. The backlight never drops below the dim level (section 6). The ring and label disappear when
  the server clears the alarm. If the alarm state changes or a new alarm starts, the full-screen takeover returns and needs
  another acknowledge.
- Other alarm types, if added later, use the same takeover until a lighter treatment is agreed.
- Takeover layout: `DRAGGING` or `FIX LOST` straight on top (20 px, red), the geofence with the anchor and the boat dot, the
  warning mark, `DRAG ALARM!` or `GPS LOST!` (28 px), the distance and radius under it for a drag alarm (`14 m OF 35 m`, 14 px),
  `PRESS TO ACKNOWLEDGE`, and the telemetry line at the bottom (only what the boat publishes, `NO GPS FIX` for GPS lost).

## 6. Power, dimming and wake

- Idle dim after 30 s to 10 %. Both values are configurable in the knob's settings because 10 % may be hard to read in direct sun.
- While the autopilot is engaged or an alarm is active the backlight never goes below the dim level. After a long idle in
  standby with no alarm it may go fully off (backlight 0), the timeout is configurable.
- Wake: the first input is **acted on** when the backlight is above 0. When the backlight is 0 the first input only wakes the
  screen and is swallowed, except an alarm acknowledge. Because 0 only occurs in standby with no alarm, an engaged autopilot
  always acts on the first input.
- Touch wakes too (decided 2026-11-23). With the backlight at 0 a touch **anywhere**, the centre dead zone included, wakes the
  screen and is swallowed. A knob push from a dark screen reaches the glass first, so a button press that starts within 0.5 s
  of such a touch wake is swallowed as well: a push from dark only wakes, as before. From the dim level any touch wakes, and
  one outside the dead zone also acts (in LIGHTS it steps the channel, the only thing touch does). A touch counts once, at
  touch-down, however long the finger stays.
- Day and night follow `environment.mode` ("day" / "night"), default Day until the first value arrives. Night uses a red
  palette and a lower brightness ceiling.
- The encoder and button must reset LVGL's inactivity counter (SPEC open question 7). The spike shows both do, polling
  `lv_display_get_inactive_time()` in a timer.

## 7. LEDs

Priority from highest: alarm, stale, state colour, turn feedback. Above all of them, the **LEDs off** setting (section 3.5)
turns every LED off, the alarm included.

- Alarm: all five red, flashing.
- Stale: slow amber pulse.
- State colour: steady dim colour for autopilot engaged and anchor dropped, nothing in standby. Night mode turns every colour into
  dim red.
- **Level patterns (#9, 2026-11-24)** in the lights and music modes, replacing the steady colour there:
  - Four LEDs show the level, a quarter of the range each, filled clockwise from the bottom left: 8:30, 11, 1 o'clock, 3:30.
    Each fades in over its quarter, so at 50 % the left two are full and the right two off. No pulsing.
  - The 6 o'clock LED: music green while playing (off when paused or stopped), lights warm white while the channel is on.
  - Lights: the selected channel, warm white. An on/off channel lights all five while on. Off: all dark.
  - Music: the selected target (master or a zone), violet-blue. A muted zone at a quarter of the brightness.
  - The level is what the server reports, not the value pending while turning, so it follows the hand with a short lag.
  - Stale data (`ZONE OFFLINE`, a channel without a value): all dark.
  - Brightness follows the brightness setting and the screen's idle dim, off with the screen. Night mode: all dark.
  - The fill is `knob_led_fill` in knob_logic, the motor mode (#8) uses it for the throttle.

## 8. Visual language

The target look is the proposal's interactive prototype (`prototype/index.html` in the handoff), not the flat mock screens
drawn first in this conversation. It reads as an instrument. Mock screens in this style: six screens (autopilot, engage
prompt, anchor preview, alarm, stale link, mode ring) were drawn and approved in conversation. They are not stored in the repo.

Elements:

- **Palette:** dark navy background (`COL_BG`), cyan primary accent, amber for caution and anchor, green and red for rudder
  sides, purple for HID, red-shifted night palette (`COL_NIGHT_RED`, `COL_NIGHT_DIM`).
- **Tick ring:** minor ticks every 5 degrees and major ticks every 30 degrees around the edge of every mode screen. The ring
  takes the state colour: light grey normally, dim cyan in the engage prompt, grey when stale, red in an alarm.
- **Autopilot:** CTS bug on the perimeter, red lubber mark at the top, large bold heading, status pill, bold CTS, delta line,
  rudder bar with a red centre tick.
- **Anchor:** the watch circle as a plot: cyan ring when dropped, boat dot with a trail, an amber ring for the radius preview (section 3.2).
- **All text is straight** (decided 2026-11-23, the owner: curved text drawn letter by letter along the rim is hard to read).
  The curved-text code is removed. Near the round edge a straight line has little room, about 130 px at y 30 and about 150 px
  at y 210, so the texts there are short and long ones end in dots.
- **Status line on top:** 20 px at y 30 on the anchor screen, the alarm screen and the boot and setup screens: `RAISED`,
  `DROPPED`, `SET RADIUS`, `DRAGGING`, `FIX LOST`, `HELM DIAL`, `JOIN WI-FI`, `CONNECTING`, `NO SERVER`, `RESET`. Longer top lines
  (the settings list position and item names) are 12 px a little lower.
- **Bottom line:** 12 or 14 px: the anchor screen's telemetry, the alarm's telemetry, the setup screens' second line, the
  settings hints (`PRESS EDIT · 2x BACK`, `PRESS STORE · 2x UNDO`, `PRESS CANCELS`, `PRESS BACK`).
- **Type:** bold throughout, large primary values. Secondary text is 12 to 13 px in the 240 px design space and is read only as a
  supporting line.
- **Left out of the prototype:** the 3D bezel (it is the physical knob ring, not part of the display).
- **Glass, glow and HUD effects:** the prototype itself is flat. For the mode ring and the alarm takeover the target is the
  glass and HUD look of the two reference mockups (translucent glass ring with a light rim, blurred live screen behind it,
  glowing icons and alarm text, segmented inscribed rings, scan lines, radar sweep). These cost frame time and RAM over SPI,
  so on the device they are pre-rendered images with alpha or cheap approximations, decided by measurement in the spike
  (open item 12).

### Readability rules

- Text is read at arm's length, in sun, on a 48 mm display. Primary values use the largest sizes that fit. Fonts are checked on
  the real panel. The prototype's muted grey for secondary text is lightened (about `#8CA0BC`) for sun contrast.
- Keep text inside a safe circle. Text at the very top and bottom edges can clip at the round bezel, the autopilot screen is
  the tightest.
- Contrast must be checked in the day and night palettes on the real panel, not only in the browser prototype.
- Night mode keeps the same layout with every colour mapped to the red palette. The tick ring dims rather than turns off.

### Style tokens (imported from the prototype v2 package, `elecrow_128_marine_ui_v2`)

The curved text in these prototype values is superseded: the firmware draws every line straight (section 8, 2026-11-23).

Values are in the 240 x 240 design space. The Canvas prototype (`prototype/index.html`) is the visual reference. Where the
package's LVGL code (`marine_ui.c`) differs from the prototype, the prototype wins and the difference is listed below.

Palette, day: background `#030811`, card `#0B1628`, ring track `#16263E`, border `#233856` (prototype buttons use `#284166`),
cyan `#00F0FF`, amber `#FFAA00`, green `#00E676`, red `#FF2A3C`, purple `#B388FF`, gold `#FFD54F`, text `#F0F6FC`, muted
`#647A96` (lightened to about `#8CA0BC` for sun contrast, see readability rules).
Palette, night: background `#050000`, every accent and text colour `#FF1E27`, muted `#7A0C10`. In the prototype green, red,
purple and gold also collapse to `#FF1E27`.

Autopilot:
- Compass ticks every 5 degrees rotate with the heading. Major ticks every 30 degrees run r 105 to 116, width 2, text colour.
  Minor ticks run r 111 to 116, width 1, muted. CTS bug is a dot of radius 5 at r 108 in the primary colour.
- Lubber triangle at (115,2), (125,2), (120,11), red.
- `HEADING (HDG)` 10 px bold muted at y 38, heading 42 px extra bold at y 74 (white), status pill rectangle x 56, y 86, 128 x 22,
  radius 11, fill primary at 14 % (amber at 14 % in standby), stroke 1.5, label 10 px bold at y 100.
- `CTS nnn°` 24 px extra bold primary at y 134, delta line 10 px muted at y 152 (`ON COURSE` at zero).
- Rudder bar: rectangle x 56, y 168, 128 x 10, radius 4, fill `#0B1628`, stroke `#233856`. The fill grows from the centre, up
  to 60 px at 35 degrees, green to starboard and red to port. Rudder text 10 px muted at y 192.

Anchor:
- Superseded by design B (section 3.2): watch circle r 72, fill card colour, stroke 2, `#284166` when raised, primary when
  dropped, red in alarm.
- Curved text on the top arc (state and distance) and bottom arc (depth, voltage, fix) at r 102, 10 px bold, centred on the top
  and bottom, character step 0.068 rad. Top text is red in alarm, primary otherwise, bottom is muted.
- Anchor glyph 22 px amber at y 114, hint 11 px bold at y 144, boat dot radius 6 at distance ratio x 74 px (clamped to 1.25),
  green, red in alarm, white 1.5 px outline.
- The 1.5 s hold progress ring (r 114, amber, width 5) is a touch gesture and is dropped. Our anchor actions use the long
  press prompt (section 3.2).

USB-HID (arc dropped, see section 3.4): status 10 px bold green at y 48, profile 20 px extra bold at y 116, last action 11 px
bold purple at y 144.

Cabin light (new in v2, not yet in our modes, see open item 16):
- Dimmer gauge: arc r 108 from 135 to 405 degrees (270 degrees), width 10 when dimmer has focus and 6 otherwise, track
  `#16263E`, value arc in the active colour (`#3A4A63` when off).
- Seven colour swatches on an orbit of r 84, 24 degrees apart, centred on the top. Selected radius 7 with a 2 px white outline,
  others radius 4.5. (The package's LVGL code uses 22 degrees, 14 and 9 px: the prototype value wins.)
- Centre orb r 44, fill the active colour at 20 % to 100 % opacity following the brightness, outline 3 px white when the dial
  controls colour, else 1.5 px in the active colour. Level text 22 px extra bold at y 122 (`OFF` when off), preset name 9 px
  bold at y 138, hints 10 px muted at y 186 (`DIAL: DIMMER` or `DIAL: COLOR`) and y 202 (`HOLD 1s: ON/OFF`).
- Presets (name, hex, R G B W as in the package): NIGHT RED `#FF1E27` 255 0 0 0, WARM WHITE `#FFCF87` 255 180 80 255, COOL WHITE
  `#E8F4FF` 200 225 255 255, AMBER DECK `#FF9800` 255 110 0 0, OCEAN CYAN `#00E5FF` 0 229 255 0, DEEP BLUE `#2979FF` 20 90 255 0,
  EMERALD `#00E676` 0 230 118 0.

Mode switcher in the package (superseded by our glass ring, section 2): overlay `rgba(2,6,13,0.92)`, a 7 px primary arc segment
at r 113 for the selected mode, `SELECT MODE` 10 px at y 44, mode name 22 px extra bold at y 122, hint 11 px primary at y 152.

Fonts: the prototype draws bold sans-serif. The firmware uses Montserrat 12, 14, 28 and 44 (`LV_FONT_MONTSERRAT_*`), so sizes in
the prototype map to the nearest of those, and a Montserrat Bold or a custom font at 10 to 13 px may be needed for the small
curved labels (to check on the panel).

Package v3 (`elecrow_128_marine_ui_v3`) adds the screens and behaviour that v2 lacked, built to our prompt. Style values it
introduces (prototype is the reference): mode ring slots at r 86 (AUTO top, LIGHT right, ANCHOR bottom, USB left), selection
brackets 54 x 34, ring shroud `rgba(4,10,20,0.78)` with a 2 px rim `rgba(220,238,255,0.35)` at r 116, clear centre r 56 with a
1.5 px rim; alarm ring a 10 px brushed-metal gradient (`#495466`, `#1B222E`, `#58657A`) at r 114, geofence r 46 centred at
(120,102) (dashed for GPS lost), boat dot red r 6 outside the circle, anchor glyph 32 px, headline 22 px, curved lines at r 98;
stale overlay `rgba(2,5,11,0.84)` with a 5 px amber ring at r 112; engage prompt overlay `rgba(3,8,17,0.94)` with a 7 px
countdown arc at r 110; muted text `#8CA0BC`. Review notes and known defects in the v3 LVGL code are in the open items
(items 13 to 15).

Not in the package (ours to supply or request): the icon-based glass mode ring, the alarm takeover screen, the stale-link
overlay, boot and pairing screens, the group-based dimmer, the HID no-host state, and a night palette beyond the all-red
mapping. The package's behaviour (tap to engage, swipe up for the switcher, press and hold to drop anchor, hold plus turn to
weigh) is not used, our interaction model in sections 2 to 5 replaces it.

## 9. Architecture notes for the build

The proposal's registry pattern (one descriptor per mode: build, refresh and input handlers) is kept. Changes:

- LVGL 9.3 API, our own `lv_port.c` (not `esp_lvgl_port`), ESP-IDF 6.1, CST816D touch.
- Encoder as an LVGL encoder indev with a group, plus button. The press is classified (single, double, long) in one input
  layer, the modes receive semantic events (turn, press, long press, mode ring), not raw keys.
- Separate state from UI: a **state snapshot** (server to knob) and an **action** (knob to server) as in SPEC section 4. UI
  code reads the snapshot and emits actions, it never writes autopilot or anchor state.
- All LVGL calls under `lv_port_lock()`.
- Touch: the circular dead zone stays for the MVP. Touch masking around pushes is a later item (SPEC section 10.2).
- The proposal's hold-and-turn weigh anchor chord and 1.5 s touch hold to drop are dropped.

## 10. Open items

1. Behaviour of turn in autopilot standby (nothing, or scroll modes).
2. Exact engage confirm turn (direction, timeout) and the prompt wording.
3. Dim defaults: 30 s and 10 % are the starting point, check in sun and at night.
4. Source paths for rudder, depth, voltage, GPS fix and autopilot mode **(verify)**.
5. Alarm acknowledge path **(verify)**.
6. How the anchor "move drop point here" maps to the anchor plugin (SPEC open question 3).
7. Whether touch dimmer control is worth the ghost-touch risk.
8. Settled 2026-11-23: no curved text at all (section 8). The per-glyph canvases are gone.
9. Boot, pairing, dimmer and HID screens exist in the v5 prototype. Still to review there: the night palette on every screen.
10. HID after a host dropout: stays on the no-host screen and waits for the user (as agreed). Revisit auto-resume if it is
    annoying in practice, for example when the host PC reboots.
11. Later: show a configured SignalK path on the HID screen (for example a value the user picks in the knob's settings).
12. Glass, glow and HUD effects (section 8): which of them ship, and how (pre-rendered alpha images or cheap approximations),
    judged by frame time and RAM on the panel.
13. RESOLVED (return to the previous mode with a persistent red ring, section 5). Alarm after acknowledge: in the v3 package the alarm overlay stays up ("ACKNOWLEDGED • WAITING") until the server clears
    the alarm, and while it is up every other input is ignored. A drag alarm can last as long as the boat is outside the
    radius, which would lock out the autopilot, dimmer and mode ring. Decide: after an acknowledge return to the previous mode
    and keep a persistent alarm indicator (red ring and LEDs), or stay on the alarm screen.
14. Stale link: the rule is decided (per-value age with `meta.timeout`, section 4), the v3 package still has to change. It
    darkens the screen but the values stay faintly visible, and staleness is a single connected flag. Required: mask the numbers
    and drive STALE per value. Open: confirm `meta.timeout` is set on the boat's paths and set the default timeouts.
15. Mode ring centre: v3 shows only "MODE / 1x SELECT". The reference shows the live mode shrunk into the centre.
17. v4 package review, fixed in a v5 package (`ui/marine_ui_v5/`, applied by the project session, not
    compiled and not run on hardware): per-glyph layer finish for the curved text, stale evaluated every tick with per-value
    `meta.timeout`, the mode ring and HID usable while stale, drawn icons, drawn step marks, rate-limited tick ring, pulsing
    acknowledged ring and LED actions. Still to verify on the panel: tick-ring frame time, icon shapes,
    arc wrap for the AUTO gauge icon. See that package's HANDOFF.md.
16. Cabin light mode from the v2 package: seven colour presets with RGBW values and a dimmer. Our dimmer mode so far is groups
    with a level and on/off. Decide whether colour presets belong in the MVP, and if so which SignalK paths carry colour.
