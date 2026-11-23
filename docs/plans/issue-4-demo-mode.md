# Plan: #4 demo mode

## Overview
A demo mode that runs the whole UI on simulated data, with no server and no network, and never sends a command anywhere. For
bench work, showing the knob away from the boat, and the manual's screenshots.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 2 (UI contract: snapshot in, actions out), 2.5 (knob_model).
- SPEC section 2 (nothing leaves the device without live control).

## Approach
- The old `demo_model.c` from the bring-up is gone. Write a `knob_demo` component that produces the same
  `marine_ui_snapshot_t` and takes the same actions as knob_model, and choose one of the two at boot.
- Simulation: heading that drifts and follows the target when engaged, a boat swinging on the anchor with a slow drag option,
  relays that switch at once, voltage and depth that move a little.
- Actions are applied locally. knob_actions is not started in demo mode, so nothing can reach a server.
- Marked on screen: a small `DEMO` tag on every mode screen, and a distinct slow LED pattern.
- Turned on from the settings page (and #3's API). Off by default, cleared by a factory reset. The serial bench commands
  (`a`, `g`, `c`, `x`, `s`, `n`) work in demo mode too.

## Test strategy
- Host tests for the simulation's pure parts if they grow logic (anchor swing, heading follow).
- Hardware checklist: demo on and off from the settings page, no network traffic in demo mode (check the server's access log).

## Open questions
- Does demo mode keep Wi-Fi up (for the settings page) or run fully offline?

## Steps
- [ ] `knob_demo` component with the snapshot builder and local actions.
- [ ] Boot-time choice in `main.c` from the config flag.
- [ ] DEMO tag in marine_ui (a snapshot flag) and the LED pattern in knob_power.
- [ ] Settings entry, docs.

## Files
New `firmware/components/knob_demo/*`, `firmware/main/main.c`, `firmware/components/marine_ui/*` (tag),
`firmware/components/knob_power/*`, `firmware/components/knob_config/*`, `docs/manual.md`.
