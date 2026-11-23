# Plan: internal RAM dip at boot (done 2026-11-23)

**Done.** Cause: Wi-Fi and LWIP receive buffers and the knob's own task stacks in internal RAM. Fixed by allowing Wi-Fi and LWIP
buffers in PSRAM and moving the task stacks (all but LVGL) to PSRAM. Low-water mark 39.8 KB, about 60 KB free. Kept as a record
until the trials confirm it over a longer run, then delete.

## Overview
Since commit 84adfd6 the boot log reports a lowest free internal RAM of 276 bytes (4,112 before, steady state about 32 KB).
espOS's health watchdog raises an alarm below 10 KB and reboots below 12 KB of free internal RAM, and a failed allocation at that
moment crashes. This has to be found and fixed before anything else is tried on the board.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 9 (risks), docs/HANDOFF.md "Internal RAM is the scarce resource".
- ARCHITECTURE.md section 2 (lv_port: LVGL on a PSRAM allocator).

## Approach
Measure first, then fix the biggest user. Suspects, most likely first:
1. The curved text canvases, now 240 x 72 x 4 bytes (69 KB each, 6 of them). They ask for PSRAM, but fall back to
   `lv_malloc`, and LVGL's draw layers for canvases may take internal RAM while drawing.
2. The anchor screen's added objects and the second overlay (`ovl_anc_res`, an `lv_arc`), all built at init in
   `marine_ui_init()` while Wi-Fi and TLS buffers are being set up at the same time.
3. Boot order: the UI is built while espOS starts Wi-Fi, so their peaks add up.

Fixes in order of preference: make sure no large buffer can fall back to internal RAM (fail loudly instead), build the UI before
Wi-Fi starts or the other way round so the peaks do not overlap, shrink the curved strips back to what the text needs (a band
around the arc instead of a full-width rectangle).

## Test strategy
- Log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and the low-water mark after each init step in `main.c` (board, LVGL,
  marine_ui, espOS start, knob_model), for one boot.
- The fix is done when the boot low-water mark is back above 12 KB with margin, over five cold boots. Hardware checklist line
  "Internal RAM low-water mark" goes to ok.

## Open questions
None, this is a fix.

## Steps
- [ ] Add per-step heap logging in `main.c` (keep it, it is cheap).
- [ ] Flash, read one boot, find the step that takes the RAM.
- [ ] Fix the cause, rebuild, five cold boots.
- [ ] Update the hardware checklist and HANDOFF.md.

## Files
`firmware/main/main.c`, `firmware/components/marine_ui/marine_ui.c`, possibly `firmware/components/lv_port/lv_port.c`.
