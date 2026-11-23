# Plan: hardening before unattended use

## Overview
The knob must not be left running with an open setup access point, an unauthenticated REST API, or someone else's USB IDs.
This collects the items that have to be done before it runs unattended on the boat or the repo goes public.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 6 (security), section 10 (own USB VID/PID).
- SPEC section 13 (token permission, question 9).
- docs/HANDOFF.md "Gotchas learned".

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
- pid.codes or another source for the USB PID (owner).
## Steps
- [x] Decide the API key: optional (owner, 2026-11-23).
- [ ] Setup through the portal on a phone, with and without a key. A key set protects the API (401 without it).
- [ ] Verify the setup AP closes.
- [ ] USB VID/PID.
- [ ] Token permission check.
- [ ] Update ARCHITECTURE.md section 6 and the manual's setup section.

## Files
`firmware/sdkconfig.defaults` (or a product profile), `firmware/components/knob_usb/usb_dev.c`, `ARCHITECTURE.md`,
`docs/manual.md`.
