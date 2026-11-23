# Plan: #2 user manual

## Overview
Finish `docs/manual.md` (draft 1 exists, anchor section updated for the drop and raise rework) from the shipped behaviour, with
photos and screenshots once the screens are checked on the panel.

## Relevant SPEC / ARCHITECTURE sections
- docs/ui-design.md (all sections), SPEC sections 2 and 7.
- The issue text lists the scope: first setup with the API key, input vocabulary, each mode, alarms, stale states, day and
  night, settings, reset, flashing, troubleshooting.

## Approach
- Write each section after its hardware checklist items are ok, so the manual never describes something untried. Items marked
  **(not verified)** in the draft stay marked until then.
- Screenshots from demo mode (#4), photos of the knob on the boat.
- Gaps the manual finds go back into ui-design.md.

## Test strategy
Someone who has not used the knob sets it up and uses each mode from the manual alone.

## Decisions
- The manual lives in `docs/manual.md`, linked from the README and from the knob's settings page (decided 2026-11-23). No PDF
  and no separate docs site for now.

## Open questions
None.

## Steps
- [x] Setup section with the optional API key, the clock fallback and reset setup (2026-11-23). The portal password is in the
      settings section.
- [ ] Autopilot and anchor sections, after the live control trials.
- [x] Settings screen and demo mode (#5, #4), volume (#6), the pilot dialog, wind and route (#7). Draft 2.
- [ ] Screenshots and photos.
- [x] README and settings page link to the manual (the knob section's description on the settings page).
- [ ] A read-through by someone new.

## Files
`docs/manual.md`, images under `docs/img/`.
