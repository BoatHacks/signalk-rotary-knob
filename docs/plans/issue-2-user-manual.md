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

## Open questions
- Where the manual lives for users who never open the repo (a page in the BoatHacks docs, a PDF, the README).

## Steps
- [ ] Setup section with the API key, after the hardening plan.
- [ ] Autopilot and anchor sections, after the live control trials.
- [ ] Settings screen and demo mode, after #5 and #4.
- [ ] Screenshots and photos.
- [ ] A read-through by someone new.

## Files
`docs/manual.md`, images under `docs/img/`.
