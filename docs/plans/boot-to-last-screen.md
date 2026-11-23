# Plan: boot to the last screen

**Built and checked 2026-11-23.** Option A was built first (0ffc8ef, mode at about 2.5 s under a `CONNECTING` overlay), then
changed on the owner's wish: the splash keeps the phases until the token is verified. Left: the anchor and USB-HID restore checks
and the stream drop about 3 s after the first connect, which happens on every boot. Delete this plan when those are settled.

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

## Decision
Option A was chosen first, with `CONNECTING` instead of `LINK LOST` while no data has arrived since boot. Seen on the panel, the
owner preferred the phases on the splash over a mode screen under an overlay (2026-11-23): the splash stays until the token is
verified, then the last mode opens with a short `CONNECTING`. That takes about 6 s, so the last screen target is 10 s now.

How it was built:
- knob_model shows the splash in every phase but setup portal, pairing and failed until the token is verified for the first
  time since boot (phase link or online, latched in `knob_boot_show_mode` in knob_logic). After that a drop shows `LINK LOST`
  over the mode instead of the splash, which is what ui-design section 4 always asked for.
- The last mode did not exist yet: marine_ui emits `MARINE_ACT_MODE_CHANGED` on a ring change, knob_config stores the last
  non-HID mode and a USB-HID flag in its own NVS namespace `knob_state`. The ring change arrives on the input task, whose
  stack is in PSRAM, so the flash write is handed to the LVGL task (internal RAM stack) with `lv_async_call`. At boot knob_model opens the stored mode, and USB-HID when it was last and a host is on the cable in
  the first 10 s (ui-design section 3.4).
- marine_ui's link overlay says `CONNECTING` with the boot phase while `last_data_ms` is 0.

## Open questions
None.

## Steps
- [x] Owner decides A or B (A, 2026-11-23, then the splash until the token is verified).
- [x] Implement in `knob_model.c` (system screen choice) and `marine_ui.c` (overlay text before first data), plus the last mode
      store (knob_config) and the boot rules with host tests (knob_logic).
- [x] Flash, measure power on to mode screen, go through the boot lines of the hardware checklist (anchor and USB-HID restore
      still open). The splash-until-token version checked on ee2b16f: 7.4 s.
- [ ] Stream drop: three boots out of three, the server closes the stream about 3 s after the first connect. Find out why
      (server log at that moment, the unsubscribe espOS sends, the device meta it publishes).
- [x] Update the gate table in ARCHITECTURE.md.

## Files
`firmware/components/knob_model/knob_model.c`, `firmware/components/marine_ui/marine_ui.c`, `firmware/components/knob_net/*`,
`firmware/components/knob_config/*`, `firmware/components/knob_logic/*`, `firmware/main/main.c`, `test/host/test_knob_logic.c`,
`ARCHITECTURE.md`, `SPEC.md`, `docs/ui-design.md`, `docs/manual.md`, `docs/hardware-checklist.md`.
