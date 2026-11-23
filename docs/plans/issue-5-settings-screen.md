# Plan: #5 settings screen on the knob

## Overview
A settings screen on the knob for what a skipper changes at the helm: display, night mode, LEDs, heading reference, HID
profile, an info page and reset setup. Everything else stays on the web settings page.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (safety rules), 7 (UI), 9 (configuration).
- docs/ui-design.md sections 2 (input vocabulary) and 4 (mode ring).
- Issue #3 plan (the same config store and reload).

## Approach
- **Opening it:** a fifth slot on the mode ring with a gear icon, only shown when no alarm is active. Leaving it: double press,
  like every mode.
- **Layout:** a list on the round screen, one item in focus in the middle, its value large. Turn moves, single press edits,
  turn changes the value, single press stores, double press goes back. A long press is not used here.
- **Items:** idle dim time, dim level, active brightness, off timeout; night mode (server, day, night); steering LEDs on or off;
  heading reference (true or magnetic); HID profile; info (version, Wi-Fi, server, token, IP, free RAM); reset setup with the
  existing hold-to-confirm.
- **Safety:** nothing here can change autopilot or anchor state or acknowledge an alarm. An alarm still takes over the screen.
- Values are written through knob_config, so the web page and the REST API see them at once.

## Test strategy
- Mockups first, agreed before coding (as for the anchor screen).
- Hardware checklist: every item stored, survives a reboot, shows on the web page.

## Open questions
- Should **live control** be switchable here, and if so with what protection (a confirm turn, a time limit, off again at
  boot)? Today it is only the serial command `l`. Owner decides after the live control trials.
- Mode ring slot or a long hold on the ring to open it?

## Steps
- [ ] Mockups of the list and the edit state, in both palettes.
- [ ] Owner agrees on the design and the live control question.
- [ ] knob_config: setters plus reload (shared with #3).
- [ ] marine_ui: settings screen and ring slot.
- [ ] Update ui-design.md and the manual.

## Files
`firmware/components/marine_ui/*`, `firmware/components/knob_config/*`, `firmware/components/knob_model/knob_model.c`,
`docs/ui-design.md`, `docs/manual.md`.
