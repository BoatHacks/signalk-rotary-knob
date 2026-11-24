# Plan: #4 demo mode

## Overview
A demo mode that runs the whole UI on simulated data, with no server and no network, and never sends a command anywhere. For
bench work, showing the knob away from the boat, and the manual's screenshots.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 2 (UI contract: snapshot in, actions out), 2.5 (knob_model).
- SPEC section 2 (nothing leaves the device without allow control).

## Approach
- The old `demo_model.c` from the bring-up is gone. Write a `knob_demo` component that produces the same
  `marine_ui_snapshot_t` and takes the same actions as knob_model, and choose one of the two at boot.
- Simulation: heading that drifts and follows the target when engaged, a boat swinging on the anchor with a slow drag option,
  relays that switch at once, voltage and depth that move a little.
- Actions are applied locally. knob_actions is not started in demo mode, so nothing can reach a server.
- Marked on screen: a small `DEMO` tag on every mode screen, and a distinct slow LED pattern.
- Turned on from the web settings page, the config API (#3) or the knob's settings screen (#5). Off by default, stored in the config so it survives a reboot, cleared by a
  factory reset (decided 2026-11-23). The serial bench commands (`a`, `g`, `c`, `x`, `s`, `n`) work in demo mode too.
- Network in demo mode: Wi-Fi and the settings page stay up so demo can be switched off from a phone, the SignalK client is
  never started (decided 2026-11-23). With no Wi-Fi set up, the setup portal runs as usual and demo still works.
- Allow control has no meaning in demo mode and is not needed for demo actions, since none leave the knob.

## Test strategy
- Host tests for the simulation's pure parts if they grow logic (anchor swing, heading follow).
- Hardware checklist: demo on and off from the settings page, no network traffic in demo mode (check the server's access log).

## Open questions
None.

## Steps
- [x] Simulation in knob_logic (`knob_demo.c`, host tested), fed into the model state, local actions. Instead of a separate
      snapshot builder, the simulation fills the model state SignalK would, so the snapshot code stays one.
- [x] Boot-time choice in knob_model from the config flag (`espos_sk_stop()` right after `espos_start()`), restart on a change.
- [x] DEMO tag in marine_ui (a snapshot flag) and the LED pattern in knob_power.
- [x] Settings entry (`DEMO MODE`, item 10), status API `demo`, docs.
- [ ] Flash and go through the hardware checklist section "Demo mode".

## Files
New `components/knob_demo/*`, `main/main.c`, `components/marine_ui/*` (tag),
`components/knob_power/*`, `components/knob_config/*`, `docs/manual.md`.
