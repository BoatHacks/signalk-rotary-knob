# Plan: #3 REST API for config and mode

## Overview
The knob's config (allow control included) and its current mode over its own HTTP API on the espOS httpd. The config part is
espOS's existing config API, the knob adds live reload, a mode endpoint and a status endpoint.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (safety rules), 4, 8 and 9 (data model, persistence, configuration), 12, 13 question 10.
- ARCHITECTURE.md sections 2.9 (knob_config), 6 (security, API key).

## What espOS already gives
Read from `espos_httpd/src/api_config.c` and `espos_config.h` (espOS 0.32):
- `GET /api/v1/config?ns=knob` and `PUT /api/v1/config` (whole device without `?ns`), protected by the API key when one is set (optional, decided 2026-11-23).
- `PUT` takes a partial document: keys not present stay as they are. Every key is validated against the descriptor
  (`knob_config/config/knob.json`: type, range, length, enum). A bad key gives 400 with the offending `ns.key` and a reason.
  The reply lists the changed keys and `restart_required` (per key, from the descriptor).
- `GET /api/v1/config` without `?ns` exports every namespace, secrets left out. That document is the export and import format
  (SPEC question 10).
- Schema versions are per namespace with registered migrations, so an old export imports into newer firmware.
- `espos_config_subscribe()` calls back on every change, which is what live reload needs.
- `GET /api/v1/config/schema` is what the espOS settings page renders its form from. The page already uses this API.

## Decisions (2026-11-23)
- **Config: espOS's API as it is.** No `/api/knob/config` wrapper and no top-level `"schema"` field, espOS's namespace versions
  do that job.
- **Knob endpoints:** `GET` and `PUT /api/v1/knob/mode`, and a read-only `GET /api/v1/knob/status`.
- **Allow control may be switched on and off over the API.** It is the stored config key `knob.allow_control` (SPEC section 2),
  so it goes through `PUT /api/v1/config` like any other key, no endpoint of its own. Switching it on takes effect at once, the
  knob shows a short `CONTROL ON` notice, and every change is logged with its source (web or API, knob, serial override).

## Approach
- **Live reload:** knob_config subscribes to its namespace and rebuilds its struct on a change. knob_model and knob_actions read
  through knob_config, so most keys apply at once. Keys that cannot (for example the HID profile while enumerated) carry
  `restart_required` in the descriptor.
- **Mode:** `PUT /api/v1/knob/mode {"mode":"autopilot"|"lights"|"anchor"|"hid"}` goes through the same entry point as the mode
  ring. It cannot engage, drop, raise or acknowledge anything. An active alarm takeover stays on top, the mode changes under it.
  A mode that is hidden in the config, or HID with no USB host, is refused with 409 and a reason.
- **Status:** `{"mode", "link", "allow_control", "demo", "alarm", "firmware"}`. No paths, tokens or addresses.
- **Allow control:** knob_actions reads the key through knob_config on every command. The serial `l` override is a runtime
  flag on top that is never stored. The notice is a marine_ui toast driven by a snapshot field, raised when the key goes on
  from anywhere but the knob itself. No permanent indicator (decided with #5).

## Test strategy
- Host test for the mode request parsing and the refusal rules (pure C in `knob_logic`).
- Manual, curl against the knob: config read and partial write with and without the key, a value out of range, an export from
  one knob imported into another, a mode switch during an alarm (the alarm stays on top), HID refused without a host, allow
  control on and off over the API with the notice on screen and the log line, still as set after a reboot.

## Open questions
None.

## Steps
- [x] Mark `restart_required` keys in `knob.json` (only the light channels need a restart).
- [x] knob_config: subscribe and rebuild on change (`knob_config_watch`, applied by knob_model on the httpd task).
- [x] `knob.allow_control` key in `knob.json` (default off), knob_actions reads it, serial `l` becomes a session override.
- [x] `/api/v1/knob/mode` and `/api/v1/knob/status` in a `knob_api` component, body rules in knob_logic with host tests.
- [x] `CONTROL ON` notice in marine_ui (mockup agreed 2026-11-23, ui-design section 4).
- [x] Document the API in SPEC section 6.2a and the manual (export and import with curl).
- [ ] Flash and go through the hardware checklist section "Config and REST API".

## Files
`firmware/components/knob_config/*`, a new `knob_api.c` (in `firmware/main` or its own component), `firmware/main/main.c`,
`firmware/components/marine_ui/*` (mode switch entry point, toast), `firmware/components/knob_logic/*`, `SPEC.md`,
`docs/manual.md`.
