# Plan: #3 REST API for config and mode

## Overview
The knob's config and its current mode over its own HTTP API, on the espOS httpd. The same versioned JSON document serves
export and import (SPEC question 10), and the on-knob settings screen (#5) and the web settings page read and write the same
store.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 4, 8 and 9 (data model, persistence, configuration), 13 question 10.
- ARCHITECTURE.md sections 2.9 (knob_config), 6 (security, API key).

## Approach
- espOS already serves `config/knob.json` through its settings page and its config REST API. First check what that gives for
  free (read, write, validation from the descriptor). Build only what is missing.
- Proposed endpoints, under the espOS API key:
  - `GET /api/knob/config`: the whole knob config as one document with `"schema": 1`.
  - `PUT /api/knob/config`: replace it. Every field validated and bounded, unknown schema rejected with a clear error, the reply
    says which fields need a restart.
  - `GET /api/knob/mode`, `PUT /api/knob/mode` with `{"mode":"autopilot"|"lights"|"anchor"|"hid"}`.
- Mode switching goes through the same path as the mode ring, so it cannot engage, drop, raise or acknowledge anything.
- Apply without a reboot where possible: knob_config gets a reload that knob_model and knob_actions already read through.

## Test strategy
- Host test for the validation and bounds (pure C in `knob_config` or `knob_logic`).
- Manual: curl against the knob with and without the key, a bad document, an unknown schema, a mode switch while an alarm is
  active (must not hide the alarm).

## Open questions
- Does the espOS config API already cover read and write well enough that only `/mode` and the export wrapper are needed?
- Does the web settings page use this API, or keep using espOS's own?

## Steps
- [ ] Read espOS's config REST API and decide what is new.
- [ ] Validation and bounds with host tests.
- [ ] Config endpoints, then mode endpoints.
- [ ] Live reload of the config without a reboot.
- [ ] Document the API in SPEC section 6 and the manual.

## Files
`firmware/components/knob_config/*`, a new `knob_api` component or a file in `knob_config`, `firmware/components/marine_ui`
(mode switch entry point), `SPEC.md`, `docs/manual.md`.
