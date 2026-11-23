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

## Lights

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Turn right switches Relay 1 on, left off | ok | 2026-11-23 | be65d7f | PUT 200, server state followed |
| A change from another client shows on the knob | ok | 2026-11-23 | be65d7f | |
| Long press toggles, single press steps to the next channel | open | | | only one channel configured |
| Dimmer channel (level arc, first click wakes) | n/a | | | no dimmers on this boat |

## Autopilot (allow control on, owner at the helm)

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Heading shown without stale errors | ok | 2026-11-23 | be65d7f | after the delta squelch override on the server |
| Heading reference label (T or M) | open | | | |
| With live control off an engage is refused and logged | ok | 2026-11-23 | be65d7f | log only, no screen feedback yet |
| Engage: long press plus confirm turn | open | | | |
| Target change, 1 and 10 degree steps, 0.4 s commit | open | | | |
| Standby with a long press | open | | | |
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
| State line (20 px) and telemetry line (14 px) straight and readable, inside the round edge | open | | | were curved 12 px, changed 2026-11-23, 14 px state looked too small. `SET RADIUS` is the widest |
| Alarm screen: `DRAGGING` / `FIX LOST` straight on top, `14 m OF 35 m` under the headline, nothing clipped | open | | | bench alarms `a` and `g` |
| Boot and setup screens: straight top line (`HELM DIAL`, `JOIN WI-FI`, `CONNECTING`, `NO SERVER`, `RESET`) inside the edge | open | | | `CONNECTING` is the widest |
| Drag alarm takes over the screen, LEDs flash, press acknowledges | open | | | bench alarm (`a`) ok on an earlier build, real alarm open |
| Acknowledge reaches the server's Notifications API | open | | | |
| GPS lost alarm while dropped | open | | | |

## Power, night and HID

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Idle dim after 30 s, off after 10 min, input wakes | open | | | |
| Screen off: a touch anywhere (centre included) wakes it and does nothing else | ok | 2026-11-23 | 8bf8ce4 | centre and edge, on the anchor screen |
| Screen off: a knob push only wakes, no click and no long press | ok | 2026-11-23 | 8bf8ce4 | the push touches the glass first |
| Screen dimmed: a touch wakes, in LIGHTS outside the centre it also steps the channel | ok | 2026-11-23 | 1350c7c | wakes on the anchor screen. Failed on 8bf8ce4, where only LIGHTS woke from dim. The channel step is not visible with one channel |
| A finger left on the glass counts once | open | 2026-11-23 | 8bf8ce4 | the screen stays awake under a resting finger (10 s, centre), probably fresh touch-downs from the controller. Counting once is not visible with one light channel |
| Night palette from `environment.mode` and the `n` override | ok | 2026-11-23 | be65d7f | override cycled auto, night, day |
| USB-HID mode shows in the ring when a USB host is on the cable | open | | | |
| Entering USB-HID mode switches the port to HID, leaving switches back to serial | open | | | |
| HID chartplotter, media and navigation profiles | open | | | |
| Touch ghost filter drops phantom touches | ok | 2026-11-23 | be65d7f | one dropped in the log |
