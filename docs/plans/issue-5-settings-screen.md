# Plan: #5 settings screen on the knob

## Overview
A settings screen on the knob for what a skipper changes at the helm: display and dimming, night mode, LEDs, heading reference,
HID profile, allow control, demo mode, the setup access point, read-only device and SignalK info, and reset setup. Everything
else stays on the web settings page. Design agreed 2026-11-23, written into docs/ui-design.md section 3.5.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (safety rules, allow control), 7 (UI), 9 (configuration), 12.
- docs/ui-design.md sections 2 (input vocabulary, mode ring), 3.5 (settings), 7 (LEDs).
- Issue #3 plan (the same config store, live reload), #4 plan (demo flag).

## Decisions (2026-11-23)
- **Opening it:** the last slot on the mode ring, gear icon, hidden while an alarm is active. Double press leaves, like every
  mode.
- **Layout (mockup agreed):** a list, the focused item in a pill in the middle with its value, the items before and after it
  above and below, `SETTINGS · n OF m` on the top arc, hints on the bottom arc. Editing shows the value large on an amber arc
  with the stored value under it (`was 80 %`). Same layout in the night palette.
- **Input:** turn moves, single press edits, turn changes the value, single press stores, double press cancels the edit or
  leaves the screen. No long press on this screen.
- **Items:**
  - Display: active brightness, dim after, dim level, off after.
  - Night mode: follow the server, day, night.
  - LEDs: on or off, for all modes. Off means **every** LED is off, the alarm included: an alarm then shows only on the screen.
  - Heading reference: true or magnetic.
  - HID profile.
  - **Allow control** (autopilot and anchor, the stored setting from SPEC section 2): off to on needs a press, then a confirm
    turn (one click right within 5 s, any press cancels), like engage. On to off is one press.
  - Demo mode: on or off (#4).
  - Setup access point: opens the espOS setup AP next to the current Wi-Fi (`espos_wifi_portal_open(true)`) so a phone can
    reach the settings page. Nothing is cleared. It closes after 10 minutes, or when the item is left with a press. It stays
    shut if `wifi.portal_enabled` is off on the web page.
  - Device info (read only): firmware version, Wi-Fi network and signal, IP, free RAM, uptime.
  - SignalK (read only): server, link state, token state (approved, waiting, none), allow control state.
  - Reset setup: clears Wi-Fi and pairing, with the existing 5 s hold-to-confirm.
- **Safety:** nothing here changes autopilot or anchor state or acknowledges an alarm. An alarm still takes over the screen.
  Allow control only permits commands, it sends none.
- Values are written through knob_config, so the web page and the REST API see them at once.

## Test strategy
- Host test for the list wrap, value bounds and the allow-control confirm sequence (knob_logic).
- Hardware checklist: every item stored and survives a reboot, shows on the web page, a change on the web page shows on the
  knob, allow control on needs the turn, LEDs off really dark during an alarm, the setup AP appears and closes after 10
  minutes, reset setup still needs the hold.

## Open questions
None.

## Steps
- [x] knob_config keys: `brightness_pct`, `night_mode`, `leds_on`, `hid_profile` next to #3's `allow_control`, setters plus
      reload. `demo` comes with #4, its item too.
- [x] knob_logic: items, value steps, list wrap, confirm turn, with host tests.
- [x] knob_power: global LED switch (above the alarm in priority) and the active brightness.
- [x] marine_ui: settings screen (list, edit, confirm turn, read-only pages, reset hold) and the ring with evenly spaced slots
      (mockup agreed 2026-11-23).
- [x] knob_model: setup AP item with the 10 minute close, reset setup wired (it only logged before), HID profile stored.
- [x] Update the manual, ui-design 2 and 3.5, ARCHITECTURE 2.7 and 2.9b.
- [ ] Flash and go through the hardware checklist section "Settings screen".

## Files
`firmware/components/marine_ui/*`, `firmware/components/knob_config/*`, `firmware/components/knob_power/*`,
`firmware/components/knob_logic/*`, `firmware/main/main.c`, `docs/ui-design.md`, `docs/manual.md`.
