# signalk-rotary-knob

Firmware that turns the [Elecrow CrowPanel 1.28" rotary display](https://www.elecrow.com/) (ESP32-S3, round 240x240
touch screen in a rotary knob) into a helm control for a boat running a [SignalK](https://signalk.org) server. Built on
[espOS](https://github.com/signalk-espOS/espOS).

Turn to choose a mode on the ring, press to open it:

- **Autopilot**: heading, wind angle or route, engage and standby with a confirm turn, target changes in the pilot's own
  steps.
- **Lights**: switch and dim the boat's dimmer channels.
- **Anchor**: watch circle with the boat's track, drop, move and raise the anchor, drag alarm.
- **Music**: volume, play, pause and track for the [signalk-jukebox](https://github.com/BoatHacks/signalk-jukebox) plugin, master and zones.
- **USB-HID**: plugged into a chartplotter or PC, the knob becomes a mouse wheel, media keys or arrow keys.
- **Settings** on the knob itself, plus a settings page and a REST API in the browser.

Night palette with the boat's `environment.mode`, dims when idle, wakes on touch. Nothing reaches the autopilot or the anchor
until **allow control** is switched on in the settings.

> **Status: pre-release.** Lights, music, settings and USB-HID are tested on the bench. Autopilot and anchor have not yet
> been used against a real autopilot on the water. Before 1.0.0, any release may change behaviour or settings.

## Installing

Get the newest release from the [Releases](../../releases) page:

- `signalk-rotary-knob-esp32s3-vX.Y.Z-merged.bin` for a first install over USB, written at offset 0:
  `esptool --chip esp32s3 write-flash 0 signalk-rotary-knob-esp32s3-vX.Y.Z-merged.bin`
- `-ota.bin` to update a knob by hand from its settings page. Knobs also check for updates on their own (stable channel
  by default, beta for pre-releases).

Then follow the [user manual](docs/manual.md): join the knob to the boat's Wi-Fi from a phone, approve its access request
in the SignalK admin UI, and pick the light channels and the autopilot.

Knobs running a release only accept updates signed with this project's key. A knob you built and flashed yourself needs
one USB flash of a release to take release updates.

## Documentation

- **Using it:** [docs/manual.md](docs/manual.md): setup, controls, modes, settings, troubleshooting.
- **How it works:** [SPEC.md](SPEC.md), [ARCHITECTURE.md](ARCHITECTURE.md), [docs/ui-design.md](docs/ui-design.md),
  [docs/hardware.md](docs/hardware.md).
- **Working on it:** [CONTRIBUTING.md](CONTRIBUTING.md) (build, tests, releases),
  [IMPLEMENTATION_CHECKLIST.md](IMPLEMENTATION_CHECKLIST.md), [docs/plans/](docs/plans/README.md),
  [docs/hardware-checklist.md](docs/hardware-checklist.md), [CHANGELOG.md](CHANGELOG.md).

## Licence

Apache-2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
