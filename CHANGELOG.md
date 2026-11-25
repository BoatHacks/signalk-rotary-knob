# Changelog

All notable changes to this firmware. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the
versions follow [Semantic Versioning](https://semver.org/). Everything before 1.0.0 may still change in incompatible ways.

Each release starts with one summary line right under its heading, at most 127 bytes: boards show it as the update's notes.
To release, set `version.txt` to the version, rename `[Unreleased]` to the version with the date, for example
`## [0.1.0] - 2026-11-30`, and run the "Cut release" workflow (CONTRIBUTING.md).

## [Unreleased]
The knob's settings are easier to use: live brightness and pill menus.

### Changed
- The brightness setting shows on the screen while it is turned, and is stored on leaving it.
- Night mode, heading, HID profile and demo mode are pill menus.
- The anchor dialog opens on Raise anchor while the anchor is dropped.

### Fixed
- The refusal says `CONTROL IS OFF`, as the manual does.

## [0.1.0] - 2026-11-25
First public pre-release: a SignalK helm knob for autopilot, lights, anchor watch and music, plus a USB-HID mode.

### Added
- Autopilot mode: engage and standby with a confirm turn, heading target, wind and route screens, tack (#7).
- Anchor mode: watch circle with the boat's track, drop, move and raise with a dialog, drag alarm.
- Lights mode: relays and dimmer channels over SignalK.
- Music mode for the signalk-jukebox plugin: master and zone volume, play, pause, next and previous (#6).
- USB-HID mode: chartplotter, media or navigation keys. The knob shows up as a USB device only in this mode.
- Motor mode: throttle with arming, soft limits and a fall back to neutral, off by default, no motor driven yet (#8).
- Settings screen on the knob (#5) with a page to switch each mode on or off, REST API for config and mode changes (#3).
- Demo mode with a simulated boat (#4), and an automatic tour of every mode with or without the inputs drawn (#10).
- LED levels for lights and music (#9).
- Night palette following `environment.mode`, or set to day or night by hand.
- Dimming and screen off after idle, wake on touch without acting on that touch.
- Signed OTA updates from this project's manifest, with rollback.
- User manual (#2).
