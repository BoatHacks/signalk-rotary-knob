# Plan: boot to the last screen

## Overview
The spike gate asks for the last used screen within 4 s of power on. Today the splash stays until the SignalK stream is up
(about 5.2 s). Separately, the stream once dropped and reconnected at 7.9 s after boot, cause unknown.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 9 (gate table).
- SPEC section 3.1 (connection states), docs/ui-design.md section 6 (stale and link lost).

## Approach
Two options, the owner decides:
- **A. Show the last screen early.** Leave the splash after the UI is ready (well under 4 s) and open the last mode with the
  link-lost overlay until the stream is up. Matches the gate and the stale rules, which already mask data that has not arrived.
  Cost: a second or two of `LINK LOST` on every boot.
- **B. Keep the splash until online, relax the gate to 10 s.** No overlay flash at boot, the progress bar explains the wait.

Recommendation: A, with a softer `CONNECTING` text instead of `LINK LOST` while no data has arrived since boot.

For the 7.9 s drop: log the espOS stream state changes with timestamps for ten boots and look for a pattern (token check,
DHCP renew, server side close).

## Test strategy
Hardware checklist: "Last screen in under 4 s" and "Stream stays up after boot".

## Open questions
- A or B (owner).

## Steps
- [ ] Owner decides A or B.
- [ ] Implement in `knob_model.c` (system screen choice) and `marine_ui.c` (overlay text before first data).
- [ ] Ten boots with stream state logging.
- [ ] Update the gate table in ARCHITECTURE.md.

## Files
`firmware/components/knob_model/knob_model.c`, `firmware/components/marine_ui/marine_ui.c`, `ARCHITECTURE.md`.
