# Plan: #6 volume control mode

## Overview
A VOLUME mode that sets the boat's audio volume over SignalK from the helm: turn for the level, press to mute, long press for
the next zone. Separate from the USB-HID media profile, which controls the computer the knob is plugged into.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (domain rules), 6.3 (SignalK interfaces), 7 (UI), 9 (configuration).
- ARCHITECTURE.md sections 2.5 (knob_model), 2.6 (adapters).
- docs/ui-design.md sections 2 (input vocabulary), 4 (mode ring), 6 (stale rules).

## Approach
- **Find the source first.** Survey the boat server for audio paths and plugin APIs (for example the signalk-jukebox plugin):
  which path carries the level, its unit (ratio or percent), whether it takes a SignalK PUT or needs a REST call, and whether
  mute, zones and track info exist. Record the result in SPEC section 13 like the earlier boat survey. **(verify)** everything
  until then.
- **Config:** volume path, mute path, zone list (name and path), step size, on knob_config like the light channels.
- **Model and adapter:** knob_model subscribes to the level, mute and track; knob_actions sends the change. Changes are
  buffered and sent after a 0.4 s pause, like the autopilot target, and the screen shows the pending value until the server
  confirms it.
- **Screen:** level arc with the number, zone name, track line on the curved bottom arc when published, a muted state. Reuse the
  lights screen's arc where possible.
- **Live control:** not required, volume is harmless. The mode can be hidden in the config like the others.

## Test strategy
- Host test for the step and clamp logic and the ratio and percent conversion (knob_logic).
- Hardware checklist: turn changes the level on the server, mute and unmute, zone switching, a change from another client
  shows on the knob, stale level blocks the controls.

## Open questions
- Which plugin and paths carry the volume on this boat (survey).
- One zone or several.
- Is there a track or source name worth showing.

## Steps
- [ ] Survey the server's audio paths and APIs, record it in SPEC section 13.
- [ ] Design pass: mockup of the screen in both palettes, agreed before coding, then docs/ui-design.md.
- [ ] knob_config fields for volume, mute and zones.
- [ ] knob_model subscriptions and snapshot fields, knob_actions adapter.
- [ ] marine_ui screen and mode ring slot.
- [ ] Hardware checklist items, manual section.

## Files
`firmware/components/marine_ui/*`, `firmware/components/knob_model/knob_model.c`, `firmware/components/knob_actions/knob_actions.c`,
`firmware/components/knob_config/*`, `firmware/components/knob_logic/*`, `test/host/test_knob_logic.c`, `SPEC.md`,
`docs/ui-design.md`, `docs/manual.md`, `docs/hardware-checklist.md`.
