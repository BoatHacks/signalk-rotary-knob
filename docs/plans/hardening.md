# Plan: hardening before unattended use

## Overview
The knob must not be left running with an open setup access point, an unauthenticated REST API, or someone else's USB IDs.
This collects the items that have to be done before it runs unattended on the boat or the repo goes public.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 6 (security), section 10 (own USB VID/PID).
- SPEC section 13 (token permission, question 9).
- docs/history/HANDOFF.md "Gotchas learned".

## Approach
- **API key:** optional (decided 2026-11-23): the boat's instrument Wi-Fi is a trusted network, the settings page and the API
  stay open by default. `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED` stays off. Check that a key set on the settings page protects the API
  (401 without it) and that the page asks for it.
- **Setup access point:** closes once Wi-Fi works and stays closed. Verify with espOS's portal state while connected.
- **Allow control:** decided 2026-11-23, a stored setting, off on a fresh knob (SPEC section 2). Check that a factory reset
  clears it.
- **USB VID/PID:** the current IDs are Espressif's test PID. Apply for a pid.codes PID (open hardware) or use the
  BoatHacks one if it exists, and change the descriptor and the docs.
- **Token permission:** the knob asks for readwrite. Check whether a narrower permission works with the paths it writes.

## Test strategy
Hardware checklist additions: setup through the portal on a phone, REST API answers 401 without the key once one is set, the
setup AP is gone while connected, the USB IDs show the new values in the device manager.

## Open questions
None.
## Steps
- [x] Decide the API key: optional (owner, 2026-11-23).
- [x] Setup through the portal on a phone with a key (2026-11-23): key first, then Wi-Fi, the API answers 401 without it. Without
      a key still to try.
- [x] Verify the setup AP closes (log `portal down` right after the join).
- [x] USB VID/PID: Espressif's ready-made TinyUSB ID for a HID-only device, 303A:4004 (owner, 2026-11-25).
- [x] Token permission check: `readwrite` is the narrowest level that allows `PUT` and the v2 calls, read from the
      signalk-server source on the boat (2026-11-23). Server ACLs can narrow it per path, that is server setup.
- [x] Update ARCHITECTURE.md section 6 and the manual's settings section (optional key, portal password).

## Files
`sdkconfig.defaults` (or a product profile), `components/knob_usb/usb_dev.c`, `ARCHITECTURE.md`,
`docs/manual.md`.
