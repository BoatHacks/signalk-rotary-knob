# Hardware checklist

Everything the host tests cannot cover, checked on the real panel and, where it matters, against the real boat. One line per
behaviour. Fill in the date and the firmware commit when an item is checked. An item that fails gets a note and stays open.

Status: **ok** checked and works, **fail** checked and broken, **open** not checked yet, **n/a** not applicable to this boat.

## Board and boot

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Display, encoder, push, touch dead zone, LEDs work | ok | 2026-11-22 | spike | |
| Boots without a reboot loop, no health watchdog reset | ok | 2026-11-23 | 84adfd6 | |
| Internal RAM low-water mark stays above 12 KB at boot | ok | 2026-11-23 | 549db49 | 39.8 KB minimum, about 60 KB free, no restart in 64 s. Was 276 bytes and health restarts, cause: Wi-Fi buffers and task stacks in internal RAM |
| Splash shows phase text and progress bar | ok | 2026-11-23 | 432c456 | |
| Pairing screen shows straight text | open | | | knob already paired, needs a fresh pairing to see |
| Online in under 10 s | ok | 2026-11-23 | 432c456 | 5.2 s |
| Last screen in under 10 s (splash with the phases until the token is verified) | ok | 2026-11-23 | ee2b16f | 7.4 s, of which Wi-Fi took 6.1 s on that boot (4.7 to 5 s on others). Target was 4 s, met on 0ffc8ef at 2.5 s with the mode under an overlay, the owner preferred the splash |
| A short `CONNECTING` over the mode after the splash, gone when the data arrives | ok | 2026-11-23 | ee2b16f | about 0.5 s, seen by the owner |
| Each mode (autopilot, lights, anchor) comes back after a reboot | open | 2026-11-23 | 0ffc8ef | autopilot (nothing stored) and lights ok, two boots each. Anchor not tried |
| USB-HID last, host on the cable: boots into USB-HID. No host: the last non-HID mode | open | | | try with a charger and with a PC |
| A stream drop while running shows `LINK LOST` over the mode, not the splash | ok | 2026-11-23 | 0ffc8ef | seen with the real drop about 3 s after online (see "Stream stays up after boot") |
| A knob without a token keeps the splash, then the pairing screen | open | | | needs a forgotten token |
| Stream stays up after boot | ok | 2026-11-23 | ee2b16f | with `time.sk_fallback` off, four boots out of four. Before: the server closed the stream on four boots out of five, because espOS unsubscribes `navigation.datetime` (its fallback clock) once SNTP sets the time, and signalk-server only accepts "unsubscribe all" and closes the stream on anything else. See ARCHITECTURE.md section 9 |
| Reflash over the USB-Serial/JTAG port, automatic reset | ok | 2026-11-23 | 549db49 | no 1200 baud reboot any more. Not possible while the USB-HID mode is on screen |
| Survives reboot and reflash with the token | ok | 2026-11-23 | 432c456 | |

## Setup and security

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Wi-Fi cleared: setup network opens, `JOIN WI-FI` on screen | ok | 2026-11-23 | ee2b16f | Wi-Fi cleared through the config API, the SignalK token stayed |
| Setup on a phone through the portal: API key first, then Wi-Fi | ok | 2026-11-23 | ee2b16f | the setup page asked for a login once the key was set |
| Setup network closes once Wi-Fi works | ok | 2026-11-23 | ee2b16f | log `portal down` right after the join |
| A set API key protects the REST API | ok | 2026-11-23 | ee2b16f | 401 without the key, 200 with it |
| Back online by itself after the setup | ok | 2026-11-23 | ee2b16f | the token check had backed off while the knob was offline (`NO SERVER` for a while), it then reconnected without a reboot |
| Setup without a key (Wi-Fi only) | open | | | the default, not tried on the phone. The key was removed again on the settings page, the API answers without it |

## Config and REST API (#3)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Allow control off after the update (new key, default off), refusal screen on anchor and autopilot | open | 2026-11-23 | fd863c9 | off after the update ok (status API). Anchor: Drop here with control off showed the red refusal screen, no call reached the server. Autopilot not tried |
| Allow control on through the config API: `CONTROL ON` notice 3 s, log line with the source | ok | 2026-11-23 | fce9406 | also from the settings page, log `ALLOW CONTROL ON (web page or API)` |
| Allow control stays as set after a reboot | ok | 2026-11-23 | fce9406 | |
| Serial `l` flips it for the session only, not stored | ok | 2026-11-23 | fd863c9 | log names the serial override, status API follows, the stored setting stayed on |
| A press dismisses the notice, a turn does not | ok | 2026-11-23 | fce9406 | the turn moved the anchor radius preview under the notice |
| Notice in the night palette, hidden by an alarm | ok | 2026-11-23 | fd863c9 | red in night, no notice over the bench alarm |
| Live reload: dim time and anchor radius apply without a restart | ok | 2026-11-23 | fce9406 | dim after 5 s and off after 1 min took effect at once. Anchor radius default not watched |
| Partial config write, a value out of range gives 400 with the key | ok | 2026-11-23 | fce9406 | `knob.dim_after_s` 2: 400 `out of range [5,3600]` |
| Export and import of the whole config | ok | 2026-11-23 | fce9406 | secrets come out masked, importing the same document changes nothing |
| `PUT /api/v1/knob/mode` switches the screen and is stored as the last mode | ok | 2026-11-23 | fce9406 | bad name 400, without the JSON content type 415 |
| Mode switch during an alarm: the alarm stays on top | ok | 2026-11-23 | fd863c9 | bench alarm `a`, status API reported `anchor_drag` |
| `hid` without a USB host gives 409 | open | | | needs the knob on a charger, a PC on the cable is a host |
| `GET /api/v1/knob/status` fields | ok | 2026-11-23 | fce9406 | |
| All of it 401 without the key when one is set | ok | 2026-11-23 | fce9406 | status, mode, config, mode PUT |
| Factory reset clears allow control | open | | | destructive, with the next fresh setup |

## Settings screen (#5)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Mode ring: slots evenly spaced, gear `SETUP` last, 5 with a USB host, 4 without, hidden during an alarm | open | | | |
| MODES page: pills readable at day and night, a press switches the mode and the ring follows, the last one on refuses with `ONE MODE STAYS ON`, the web page shows the same | open | | | |
| A mode switched off while on screen (web page) goes to the first one on, a boot into a switched-off mode too | open | | | |
| List: turn moves and wraps, prev and next items above and below, `SETTINGS 3/14` | open | | | |
| All text straight: settings top and bottom lines, alarm telemetry, setup screens' bottom line, nothing clipped | open | | | curved text removed 2026-11-23 |
| Edit: amber arc, value, `was ...`, press stores, double press cancels | open | | | |
| Each value stored, shown on the web page at once, still there after a reboot | open | | | brightness, dim after, dim level, off after, night, LEDs, heading, HID profile |
| A change on the web page shows on the knob | open | | | |
| Brightness changes the active backlight at once, dim level never brighter | open | | | |
| LEDs off: every LED dark, also during an alarm | open | | | bench alarm `a` |
| Night mode setting: server, day, night, serial `n` still overrides for the session | open | | | |
| Allow control off to on: press, then one click right within 5 s, any press cancels, no CONTROL ON notice | open | | | |
| Allow control on to off: one press | open | | | |
| Setup access point opens next to Wi-Fi, counts down, closes after 10 min or with a press | open | | | |
| Device info and SignalK pages show sensible values | open | | | |
| Reset setup: needs the full 5 s hold, early release does nothing | open | | | destructive, with the next fresh setup |
| Settings usable without a SignalK link | open | | | |
| HID profile from the HID long press survives a reboot | open | | | needs a USB host |

## Demo mode (#4)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Demo on from the knob's settings and from the web page: restart, `DEMO` tag, no extra LED pattern | open | | | |
| No SignalK traffic in demo mode (server access log) | open | | | |
| Autopilot: engage, target change, the heading follows, standby | open | | | |
| Anchor: drop, swing, radius below the swing raises the drag alarm, acknowledge, raise | open | | | |
| Lights switch, USB-HID still works | open | | | |
| Demo off from the web page restarts into normal mode | open | | | |

## Music (#6)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| MUSIC on the ring after ANCHOR, off the ring with `knob.music_mode` off, the API answers 409 then | open | | | |
| Turn changes the master volume on the server, 2 % a click, amber until confirmed | open | | | |
| Long press and a tap on the name step through MASTER and the zones, zone names from the plugin | open | | | |
| A zone's volume follows the turn | open | | | |
| Press plays and pauses, touch previous, next, play or pause | open | | | |
| A change from the jukebox web UI shows on the knob, track and artist | open | | | |
| A touch in the centre or on the outer arc does nothing | open | | | |
| No jukebox or stale volume: `NO VOLUME`, a disconnected zone: `ZONE OFFLINE`, turn ignored | open | | | |
| LEDs: volume in violet-blue, at 50 % the left two full, above 50 % the 1 o'clock LED fades in (#9) | ok | 2026-11-24 | 721471a | colours kept |
| LEDs: 6 o'clock green while playing, off when paused; a muted zone's level dimmed; offline zone all dark | open | | | |
| LEDs follow the reported volume after a turn, and a change from the jukebox web UI | open | | | |

## Automatic demo (#10)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| `DEMO MODE` `AUTO` from the knob and the web page, and serial `p`: countdown, splash with the boot phases, then the tour | open | | | |
| Every step does what the script says: engage and 23° to port, dim 80 to 20 % and off, radius 20 m and drop, volume 42 to 100 %, drag alarm, acknowledge, radius 30 m clears it, USB to media, setup to demo off | open | | | |
| Every turn, press, double press, long press and touch is drawn, caption readable, at day and at night | open | | | |
| `QUIET`: the same tour with nothing drawn on top and no captions, only the countdown | open | | | |
| `Eläkeläiset` shows with the umlauts, the position counts up from 0:53 | open | | | |
| Three runs in a row without a stuck step | open | | | |
| Real input pauses it, 60 s later it starts again from the top | open | | | |
| Nothing written to flash (last mode and HID profile unchanged afterwards), no USB device, no SignalK traffic | open | | | |

## Release and OTA (docs/plans/release.md)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| A release's `-merged.bin` written at 0 on an erased board boots to the setup portal, web UI served | open | | | |
| First boot writes the project manifest URL into `ota.manifest_url`, an emptied URL stays empty after a restart | open | | | |
| A knob on the beta channel is offered the next pre-release and installs it, settings and token kept | open | | | |
| An image signed with another key is refused, the knob keeps running | open | | | |
| An image that crashes before it reaches the network rolls back to the previous one | open | | | |
| `GET /api/v1/system/ping` reports app `signalk-rotary-knob` and the release version | open | | | |

## Motor (#8)

First in demo mode (a simulated motor), then against a test plugin, then with the real motor at the boat.

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| MOTOR on the ring just left of AUTO only with `knob.motor_mode` on | open | | | |
| Ring order MOTOR, AUTO (12 o'clock), ANCHOR, LIGHTS, MUSIC, USB, SETUP; a switched-off mode leaves a gap-free ring in the same order | open | | | |
| Arm: long press, one click right within 5 s, countdown arc; left click or timeout cancels | open | | | |
| Arming refused with `SET NEUTRAL FIRST`, `CONTROL IS OFF`, `NO MOTOR DATA` | open | | | |
| A spin that confirms does not raise the throttle (first 500 ms ignored) | open | | | |
| Steps: slow 1 %, normal and fast bigger per preset; the arc follows | open | | | |
| From ahead the throttle stops at N, after 1 s still it goes on into astern, and back | open | | | |
| Long press out of N: 0 at once (while holding), long press in N: disarm, single press nothing | open | | | |
| Limit at 50 %: stops, warning, amber LED flash on every screen; double press lifts both, raises nothing | open | | | |
| Tag `F 40 %` / `R 15 %` on other screens, not on the alarm; screen does not dim while out of N | open | | | |
| LEDs: ahead green 1 o'clock then 3:30, astern amber 11 then 8:30, bottom LED the gear, dark at night | open | | | |
| Throttle PUT on change and every 1 s while out of N, one 0 on a disarm, nothing after a reboot | open | | | needs a test plugin or server log |
| Link loss, stale gear (3 s), `Fault`, gear not following (3 s), control off: 0 and disarm | open | | | |
| Idle 60 s in N disarms, 0 = never | open | | | |
| At the boat, before connecting the motor: the motor side goes to neutral without the keepalive | open | | | needs the motor plugin |

## Lights

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Turn right switches Relay 1 on, left off | ok | 2026-11-23 | be65d7f | PUT 200, server state followed |
| A change from another client shows on the knob | ok | 2026-11-23 | be65d7f | |
| Long press toggles, single press steps to the next channel | open | | | only one channel configured |
| Dimmer channel (level arc, first click wakes) | n/a | | | no dimmers on this boat |
| LEDs: an on channel lights all five warm white, off all dark (#9) | ok | 2026-11-24 | 721471a | colours kept |
| LEDs: dark at night, dark with the LEDs setting off, dim with the screen's idle dim, an alarm takes over | open | | | music too |

## Autopilot (allow control on, owner at the helm)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Heading shown without stale errors | ok | 2026-11-23 | be65d7f | after the delta squelch override on the server |
| Heading reference label (T or M) | open | | | |
| With live control off an engage is refused and logged | ok | 2026-11-23 | be65d7f | log only, no screen feedback yet |
| Engage: long press plus confirm turn | open | | | |
| Target change, 1 and 10 degree steps, 0.4 s commit | open | | | |
| Standby: long press opens the pilot dialog, one press on STANDBY | open | | | changed with #7 |
| Engage sends `PUT state auto`, also after a wind or route session | open | | | |
| Target change as `target/adjust` steps (+10, +1, ...), -1 and -10 arrive as such | open | | | the provider's rounding, see the trials plan |
| Pilot dialog: COMPASS, WIND, ROUTE change the pilot's state | open | | | needs the pilot wired |
| Tack port and starboard with the confirm turn towards the side | open | | | |
| Wind screen: AWA with P/S, target, cyan line, `ADJUST ON PILOT`, turn does nothing | open | | | |
| Route screen: XTE, steer side and bar, bearing and distance, turn does nothing | open | | | needs a course set |
| Stale wind or route: `NO WIND` / `NO ROUTE`, the dialog still offers STANDBY | open | | | |
| LEDs follow the wind error and the route's steer side | open | | | |
| Steering LEDs: red, green, yellow, blue, night fade | open | | | |
| Losing the knob does not change the pilot | open | | | unplug while engaged |

## Anchor (allow control on, at anchor or on the mooring)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Raised screen: grey ring, boat in the centre, hints | open | | 84adfd6 | |
| Radius preview: amber ring, number, press applies | open | | 84adfd6 | |
| Long press dialog shows its items and the highlight | open | | 84adfd6 | |
| Live control off: red NOT SENT screen with the reason | open | | 84adfd6 | |
| Drop here: spinner, then the dropped screen | open | | 84adfd6 | |
| Dropped screen: dot, trail, distance label placement | open | | 84adfd6 | |
| Move drop point here | open | | 84adfd6 | |
| Raise anchor | open | | 84adfd6 | |
| Radius change while dropped goes to the server | open | | 84adfd6 | |
| State line (20 px) and telemetry line (14 px) straight and readable, inside the round edge | ok | 2026-11-23 | 072dc9b | were curved 12 px, 14 px state looked too small |
| Alarm screen: `DRAGGING` / `FIX LOST` straight on top, `14 m OF 35 m` under the headline, nothing clipped | ok | 2026-11-23 | 072dc9b | bench alarms `a` and `g` |
| Boot and setup screens: straight top line (`HELM DIAL`, `JOIN WI-FI`, `CONNECTING`, `NO SERVER`, `RESET`) inside the edge | open | | | `CONNECTING` is the widest |
| Drag alarm takes over the screen, LEDs flash, press acknowledges | open | | | bench alarm (`a`) ok on an earlier build, real alarm open |
| Acknowledge reaches the server's Notifications API | open | | | |
| GPS lost alarm while dropped | open | | | |

## Power, night and HID

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Idle dim after 30 s, off after 10 min, input wakes | open | | | |
| No LED flicker with all LEDs dark (LIGHTS, MUSIC) or in a steady colour | open | | | one LED flickered: the RMT frame was refilled mid-frame (64 symbols for 121), now 144 and sent only on a change, plus every 5 s |
| Screen off: a touch anywhere (centre included) wakes it and does nothing else | ok | 2026-11-23 | 8bf8ce4 | centre and edge, on the anchor screen |
| Screen off: a knob push only wakes, no click and no long press | ok | 2026-11-23 | 8bf8ce4 | the push touches the glass first |
| Screen dimmed: a touch wakes, in LIGHTS outside the centre it also steps the channel | ok | 2026-11-23 | 1350c7c | wakes on the anchor screen. Failed on 8bf8ce4, where only LIGHTS woke from dim. The channel step is not visible with one channel |
| A finger left on the glass counts once | open | 2026-11-23 | 8bf8ce4 | the screen stays awake under a resting finger (10 s, centre), probably fresh touch-downs from the controller. Counting once is not visible with one light channel |
| Night palette from `environment.mode` and the `n` override | ok | 2026-11-23 | be65d7f | override cycled auto, night, day |
| USB-HID mode shows in the ring when a USB host is on the cable | open | | | |
| Entering USB-HID mode switches the port to HID, leaving switches back to serial | open | | | |
| HID chartplotter, media and navigation profiles | open | | | |
| Touch ghost filter drops phantom touches | ok | 2026-11-23 | be65d7f | one dropped in the log |
