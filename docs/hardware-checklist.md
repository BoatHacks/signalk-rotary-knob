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
| Last screen in under 4 s | open | | | was fail on 432c456 (splash waited for the stream). Option A built: a paired knob opens its last mode when espOS reports the token. Measure power on to mode screen |
| Boot shows `CONNECTING` with the phase over the mode, gone when the data arrives | open | | | |
| Each mode (autopilot, lights, anchor) comes back after a reboot | open | | | stored on a mode ring change, log line `boot mode` |
| USB-HID last, host on the cable: boots into USB-HID. No host: the last non-HID mode | open | | | try with a charger and with a PC |
| A stream drop while running shows `LINK LOST` over the mode, not the splash | open | | | bench command `x` only fakes it, stop the server or Wi-Fi |
| A knob without a token keeps the splash, then the pairing screen | open | | | needs a forgotten token |
| Stream stays up after boot | ok | 2026-11-23 | 549db49 | stayed up for 64 s. The earlier drops came with the RAM starvation. Separate: espOS sends a single-path unsubscribe that this server rejects with an error |
| Reflash over the USB-Serial/JTAG port, automatic reset | ok | 2026-11-23 | 549db49 | no 1200 baud reboot any more. Not possible while the USB-HID mode is on screen |
| Survives reboot and reflash with the token | ok | 2026-11-23 | 432c456 | |

## Lights

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Turn right switches Relay 1 on, left off | ok | 2026-11-23 | be65d7f | PUT 200, server state followed |
| A change from another client shows on the knob | ok | 2026-11-23 | be65d7f | |
| Long press toggles, single press steps to the next channel | open | | | only one channel configured |
| Dimmer channel (level arc, first click wakes) | n/a | | | no dimmers on this boat |

## Autopilot (live control on, owner at the helm)

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

## Anchor (live control on, at anchor or on the mooring)

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
| Curved lines no longer clipped at the ends | open | | 84adfd6 | strips are 72 px tall |
| Drag alarm takes over the screen, LEDs flash, press acknowledges | open | | | bench alarm (`a`) ok on an earlier build, real alarm open |
| Acknowledge reaches the server's Notifications API | open | | | |
| GPS lost alarm while dropped | open | | | |

## Power, night and HID

| Item | Status | Date | Commit | Notes |
|---|---|---|---|---|
| Idle dim after 30 s, off after 10 min, input wakes | open | | | |
| Night palette from `environment.mode` and the `n` override | ok | 2026-11-23 | be65d7f | override cycled auto, night, day |
| USB-HID mode shows in the ring when a USB host is on the cable | open | | | |
| Entering USB-HID mode switches the port to HID, leaving switches back to serial | open | | | |
| HID chartplotter, media and navigation profiles | open | | | |
| Touch ghost filter drops phantom touches | ok | 2026-11-23 | be65d7f | one dropped in the log |
