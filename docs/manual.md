# Rotary knob user manual

> DRAFT 1, written 2026-11-23 from the implemented behaviour and docs/ui-design.md. Items marked **(not verified)** have not been
> tried on the real hardware yet. Photos and screenshots will be added once the UI has been checked on the panel.

The knob is a helm-side control surface for a SignalK boat: it steers the autopilot, switches lights and watches the anchor. It has
one round screen, an endless dial with a push button, five LEDs around the edge, and a touch screen that is only a helper.

## 1. First setup

1. **Power.** Connect 5 V (1 A) to the knob. It starts by itself and is meant to stay on.
2. **Wi-Fi.** On the first start the knob shows `JOIN WI-FI` and opens its own network, named `espOS-` followed by four characters. Join it
   from a phone and open the setup page (`http://192.168.4.1`), choose your network and enter the password. The setup network is
   open, so do this at home or on the boat and not in a crowded marina.
3. **Pairing.** The knob finds the SignalK server on its own, or you give the server address on the settings page. It then shows
   `PAIRING, WAITING FOR APPROVAL`. In the SignalK admin UI open Security, Access Requests, find the request named after the knob and
   approve it with **read/write** permission. This is done once.
4. **Online.** The splash picture shows the phases (`WI-FI`, `SIGNALK`, `TOKEN`) with a progress bar. Once the knob is paired, later
   starts skip most of that: it opens the screen you used last after about two seconds and says `CONNECTING` until the data is
   there, about five seconds after power on. The controls work from then on.

If the knob is moved to a different network, or its approval is removed on the server, it goes through the steps again.

## 2. Using the knob

| Input | What it does |
|---|---|
| Turn | Changes the value on the screen (target heading, lights, anchor radius) |
| Single press | Confirms, or steps to the next item (next light channel) |
| Double press | Opens the mode ring: turn to pick a mode, press to switch |
| Long press (about 0.75 s) | State changes: engage the autopilot, switch a light on or off |
| Touch | Secondary and never changes autopilot or anchor state. A touch in the centre is ignored (a push on the knob presses the glass there) |

Safety rules: the autopilot is engaged with a long press **and** a confirm turn. A bumped dial cannot engage anything, and losing
the knob (unplugged, Wi-Fi gone) never changes the autopilot or the anchor.

## 3. Modes

### Autopilot
The screen shows the heading in large digits with its reference (`T` true or `M` magnetic, chosen in the settings), a ring of heading
ticks, and the course to steer (`CTS`).
- **Engage:** a long press opens the confirm prompt, then turn right within 5 seconds to confirm.
- **Change the target (while engaged):** turn. A pause of about 0.4 s sends it. A single press switches the step between 1 and 10 degrees.
- **Standby:** a long press while engaged.
- The LEDs show the steering error while this screen is open: **red** means turn to port, **green** to starboard, **yellow** on course
  while engaged, **blue** on course in standby. On course means within 2 degrees (it stays on course until the error passes 3).
  At night the on-course colour fades out after 2 seconds, red and green stay.
- No rudder angle or depth is shown unless your boat publishes them (the display shows `---`).

### Lights
Channels are set on the settings page (name, SignalK path, relay or dimmer). The boat this was built for has relays only, so a channel
is on or off:
- **Turn right** switches the channel on, **turn left** off. One click is enough.
- **Single press** picks the next channel, **long press** toggles the selected one.
- A dimmer channel shows a level arc and changes in 5 % steps. A switched-off dimmer keeps its level: the first click only turns it
  back on at that level.

### Anchor
The watch circle is drawn as a plot: the ring is the radius you chose, the green dot is the boat, the anchor is in the centre, and
a short trail shows where the boat has been. The distance is written next to the dot. The top line says `DROPPED` or `RAISED`.
With the **hoekens-anchor-alarm** plugin installed:
- **Turn** changes the radius (a new amber ring shows the preview), a **press** applies it.
- **Long press** opens a dialog with only the actions that fit: **Drop here** while the anchor is raised, **Move drop point here**
  and **Raise anchor** while it is dropped, and always **Cancel**. Turn to choose, press to confirm. A radius you turned to but
  did not apply is used when you drop.
- After you choose, the screen shows a spinner until the server confirms. If the knob could not do it, it says why (live control
  off, no link, no position fix, the server refused) and that nothing was changed.
Anchor and autopilot commands are only sent when **live control** is on (see section 6).

### USB-HID
Listed in the mode ring only while a computer has recognised the knob over USB. The dial then acts as an input device for that computer:
chartplotter profile (mouse wheel zoom, push is a click), media profile (volume, push is mute), navigation keys (arrow keys, push is Enter).
**(not verified)**

## 4. Alarms

An anchor drag or GPS-lost alarm takes over the screen: the backlight goes to full, the five LEDs flash red, the screen says what
happened. A press acknowledges it. The knob then returns to the mode you were in and keeps a slow red pulse and a red ring until the server
clears the alarm. The knob never decides an alarm itself, the server does.

The GPS-lost alarm is raised while the anchor is armed and the position stops updating for 30 seconds or the fix is lost.

## 5. When data is missing

Every value has its own age. If something the screen depends on stops arriving, the knob says so (`NO HEADING`, `NO AP STATE`,
`LINK LOST`, or `CONNECTING` right after a start) and ignores that mode's controls. It never shows old values as current. The mode ring and the alarm acknowledge still
work. Values the boat does not publish show `---`.

## 6. Settings and live control

- **Settings page.** Open `http://<knob name>.local` in a browser on the same network. The *Rotary knob* section has the autopilot id,
  the heading reference, the default anchor radius, the idle dim time, the dim level, the time until the backlight goes off, and six
  light channels. Changes apply after a restart.
- **Live control.** For safety the autopilot and anchor commands are ignored after every start. Send the single character `l` over the
  USB serial port to switch live control on (and again to switch it off). It does not survive a restart. **(an on-device way to
  enable it is planned)**
- **Night and day.** The knob follows the SignalK path `environment.mode`. It starts in day mode until the first value arrives.
- **Dimming.** After 30 seconds without input the backlight drops to 10 %, after 10 minutes it goes off, but never while the autopilot
  is engaged or an alarm is active. Any input wakes it.

## 7. Updating the firmware

Updates are made over USB (over-the-air updates come later). Connect the knob to a computer. A running knob reboots into the bootloader
when its USB serial port is opened at 1200 baud, or hold BOOT and tap RESET. The repository's `tools/build.ps1` builds and flashes.
See docs/HANDOFF.md for the details and the pitfalls.

## 8. Troubleshooting

| What you see | What it means |
|---|---|
| `JOIN WI-FI` | No Wi-Fi is set up. Join the knob's setup network and enter your Wi-Fi |
| Stuck on `SIGNALK` | Wi-Fi works but no SignalK server was found. Set the server address on the settings page, or check that the server advertises itself on the network |
| `PAIRING` for a long time | The request has not been approved yet in the SignalK admin UI |
| `NO SERVER` | The server refused or cannot be reached. The knob keeps retrying |
| Controls do nothing and the log says live control is off | Autopilot and anchor commands are off after every start, see section 6 |
| The screen is dim | Idle dim, any input wakes it |
| Phantom touches | The touch controller sometimes reports ghost touches, the knob drops them |
