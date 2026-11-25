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
- **Motor**: throttle with arming, soft limits and a fall back to neutral, for a motor controller that takes a SignalK `PUT`.
  Off by default, no motor has been driven yet.
- **USB-HID**: plugged into a chartplotter or PC, the knob becomes a mouse wheel, media keys or arrow keys.
- **Settings** on the knob itself, plus a settings page and a REST API in the browser.

A demo mode runs all of it on a simulated boat, by hand or as an automatic tour. Night palette with the boat's
`environment.mode`, dims when idle, wakes on touch. Nothing reaches the autopilot, the anchor or a motor until **allow
control** is switched on in the settings.

> **Status: early release.** Setup, the SignalK link, the REST API, switching a relay and installing an update over the air have been checked on hardware. Much of the
> rest, the settings screen, music and USB-HID among it, is built but not fully checked, and autopilot,
> anchor and motor have not been used on a boat: see the [hardware checklist](docs/hardware-checklist.md). Do not rely on
> it for steering, anchor watch or throttle. Before 1.0.0, any release may change behaviour or settings.

## Installing

Get the newest release from the [Releases](../../releases) page:

- `signalk-rotary-knob-esp32s3-vX.Y.Z-merged.bin` for a first install over USB, written at offset 0:
  `esptool --chip esp32s3 write-flash 0 signalk-rotary-knob-esp32s3-vX.Y.Z-merged.bin`
- `-ota.bin` for updates over the air. Knobs check for them once a day and show them on the settings page (stable channel by
  default, set it to `beta` for pre-releases). Its link can also be pasted there.

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
