# Plan: #6 volume control mode

## Overview
A VOLUME mode for the boat's music from the helm, driving the `signalk-jukebox` plugin over SignalK: the knob turns the volume,
press plays or pauses, hold steps through master and zones, and the touch screen is a small music player (previous, next,
play/pause, zone). Separate from the USB-HID media profile, which controls the computer the knob is plugged into. Design agreed
2026-11-23, written into docs/ui-design.md section 3.6.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (domain rules), 6.3 (SignalK interfaces), 7 (UI), 9 (configuration), 12.
- ARCHITECTURE.md sections 2.5 (knob_model), 2.6 (adapters).
- docs/ui-design.md sections 2 (input vocabulary, touch rules), 3.6 (volume), 4 (stale rules).

## Survey of the boat server (2026-11-23)
`signalk-jukebox` 0.2.2 (Mopidy and Snapcast) is installed. Read from its source and the live server:
- `entertainment.jukebox.playback.volume`: master volume, ratio 0 to 1, accepts `PUT`.
- `entertainment.jukebox.zones.<id>.volume` (ratio, `PUT`), `.muted` (boolean, `PUT`), `.connected`, `.n2kZone`. Zone ids are
  Snapcast client ids, found at run time. There is no master mute.
- `entertainment.jukebox.playback.state` (`stopped`, `playing`, `paused`) and `.track` (object `{name, artist?, album?}`, only
  while a track is loaded).
- `entertainment.jukebox.playback.controls.play`, `.pause`, `.next`, `.previous`: momentary, `PUT` 1 then 0.
- Zone names: `GET /plugins/signalk-jukebox/api/zones` returns `[{id, name, connected, volume, muted, ...}]`. Unauthenticated
  reads are refused, the knob's token should be enough **(verify)**. The REST volume there is 0 to 100, the knob uses the
  SignalK paths for volume and the REST call only for the names.
- The boat has one zone today (the server's local Snapclient).
- No `meta.timeout` on these paths. Master volume is sent on change only, zones are republished every 2 s.

## Decisions (2026-11-23)
- **Knob:** turn changes the volume of the selected target by **2 %** per click. Single press is play or pause. Long press steps
  to the next target: Master, then each zone. No track skip on the knob.
- **Touch (the player):** previous and next on the left and right, play or pause at the bottom, the zone name at the top (tap
  for the next target). All targets sit outside the 50 px centre dead zone. The outer arc is display only, touching it does
  nothing.
- **Screen:** the outer arc shows the volume and nothing else. Inside: zone name, track and artist, the volume number in the
  centre. Mockup agreed.
- **Zone names** come from the plugin's REST API, fetched after connect and when a new zone id appears. A zone without a name
  shows its id.
- **Allow control is not needed**, volume and playback are harmless. The mode can be hidden in the config like the others.

## Approach
- **Model:** knob_model subscribes to `playback.volume`, `playback.state`, `playback.track` and `zones.*` (volume, muted,
  connected). knob_actions fetches the zone names.
- **Sending:** volume changes are buffered and sent after a 0.4 s pause, like the autopilot target. The screen shows the pending
  value until the server's value matches. Play, pause, next and previous are a `PUT` 1 then 0 on the control path.
- **Stale:** the selected target's volume is the essential value. The master volume has no age timeout (event driven), only link
  loss. A zone that reports `connected: false` shows `ZONE OFFLINE` and ignores the turn. `playback.track` as an object value
  through espOS's SignalK client **(verify)**.
- **Config:** mode visibility only. No paths to enter: the jukebox paths are fixed by the plugin.

## Test strategy
- Host test in `knob_logic`: 2 % steps, clamp at 0 and 100, ratio conversion, the target cycle (Master, zones, wrap).
- Hardware checklist: turn changes the master and a zone on the server, press plays and pauses, touch previous and next, zone
  tap and hold cycle the same list, a change from the jukebox web UI shows on the knob, stale and zone offline block the turn,
  a touch in the centre does nothing.

## Open questions
None.

## Steps
- [ ] knob_model subscriptions and snapshot fields (target list, volumes, state, track).
- [ ] knob_actions: volume `PUT` with the pause, control `PUT` pulses, zone name fetch.
- [ ] knob_logic: steps, clamp, target cycle, with host tests.
- [ ] marine_ui: volume screen with touch targets, ring slot.
- [ ] Hardware checklist items, manual section.

## Files
`firmware/components/marine_ui/*`, `firmware/components/knob_model/knob_model.c`, `firmware/components/knob_actions/knob_actions.c`,
`firmware/components/knob_config/*`, `firmware/components/knob_logic/*`, `test/host/test_knob_logic.c`, `SPEC.md`,
`docs/ui-design.md`, `docs/manual.md`, `docs/hardware-checklist.md`.
