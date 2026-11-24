# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
versions follow [Semantic Versioning](https://semver.org/). Everything before 1.0.0 may still change in incompatible ways.

Each release starts with one summary line right under its heading, at most 127 bytes: boards show it as the update's notes.
To release, rename `[Unreleased]` to the version with the date, for example `## [0.1.0] - 2026-11-30`, and run the
"Cut release" workflow (CONTRIBUTING.md).

## [Unreleased]
First public pre-release: a SignalK helm knob for autopilot, lights, anchor watch and music, plus a USB-HID mode.

### Added
- Autopilot mode: heading or wind angle, engage and standby with a confirm turn, wind and route modes (#7).
- Lights mode: dimmer channels switched and dimmed over SignalK.
- Anchor mode: watch circle with the boat's track, drop, move and raise with a confirm dialog, drag alarm.
- Music mode for the signalk-jukebox plugin: master and zone volume, play, pause, next and previous (#6).
- USB-HID mode: chartplotter, media or navigation keys. The knob shows up as a USB device only in this mode.
- Settings screen on the knob (#5), REST API for config and mode changes (#3), demo mode with a simulated boat (#4).
- Night palette following `environment.mode`, or set to day or night by hand.
- Dimming and screen off after idle, wake on touch without acting on that touch.
- Signed OTA updates from this project's manifest, with rollback.
- User manual (#2).
