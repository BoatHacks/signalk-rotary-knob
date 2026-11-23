# Plan: hardening before unattended use

## Overview
The knob must not be left running with an open setup access point, an unauthenticated REST API, or someone else's USB IDs.
This collects the items that have to be done before it runs unattended on the boat or the repo goes public.

## Relevant SPEC / ARCHITECTURE sections
- ARCHITECTURE.md section 6 (security), section 10 (own USB VID/PID).
- SPEC section 13 (token permission, question 9).
- docs/HANDOFF.md "Gotchas learned".

## Approach
- **API key:** product builds set `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED` (already prepared, commented, in `sdkconfig.defaults`).
  The key is set on the setup page together with Wi-Fi. Check that the portal can take the key in the same step, otherwise
  the setup has a window without one.
- **Setup access point:** closes once Wi-Fi works and stays closed. Verify with espOS's portal state while connected.
- **Live control default:** stays off at every boot until the trials are done. After that, decide whether it is a setting.
- **USB VID/PID:** the current IDs are Espressif's test PID. Apply for a pid.codes PID (open hardware) or use the
  BoatHacks one if it exists, and change the descriptor and the docs.
- **Token permission:** the knob asks for readwrite. Check whether a narrower permission works with the paths it writes.

## Test strategy
Hardware checklist additions: REST API answers 401 without the key, the setup AP is gone while connected, the USB IDs show the
new values in the device manager.

## Open questions
- pid.codes or another source for the USB PID (owner).
- Is live control a setting after the trials, or always off at boot (owner)?

## Steps
- [ ] Enable `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED` in a product build profile, test setup with the key.
- [ ] Verify the setup AP closes.
- [ ] USB VID/PID.
- [ ] Token permission check.
- [ ] Update ARCHITECTURE.md section 6 and the manual's setup section.

## Files
`firmware/sdkconfig.defaults` (or a product profile), `firmware/components/knob_usb/usb_dev.c`, `ARCHITECTURE.md`,
`docs/manual.md`.
