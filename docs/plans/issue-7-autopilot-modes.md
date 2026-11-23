# Plan: #7 wind and route modes for the autopilot

## Overview
Add the pilot's wind (vane) and route modes to the autopilot screen, next to compass steering (`auto`), so the helm can steer
to the apparent wind or follow a route from the knob. Same safety rules as compass mode.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (domain rules), 6.3 (autopilot v2 API), 12 and 13.
- ARCHITECTURE.md sections 2.5 (knob_model), 2.6 (knob_actions and adapters), 3.1 (path map).
- docs/ui-design.md sections 3.1 (autopilot), 4 (stale rules), 7 (LEDs).
- docs/plans/live-control-trials.md: compass mode is proven on the boat first.

## What the provider actually does
Read from the source of `@signalk/signalk-autopilot` 2.6.0 on the boat server (the plugin behind the `raymarineST` id, which
drives a SeaTalk 1 pilot through `$STALK` datagrams) and from the live v2 API, 2026-11-23:

- The pilot's modes are API **states**, not modes: `standby`, `auto`, `wind`, `route` (`engaged` true for the last three).
  `GET .../mode` and `PUT .../mode` answer "Not implemented", `options.modes` and `options.actions` are empty.
- **Engage** (`POST .../engage`) resumes the **last engaged state** (`lastState || defaultEngagedMode`). After a wind or route
  session it engages wind or route, not compass. Selecting a state is `PUT .../state {"value":"wind"}` and so on.
- **Target in auto:** `PUT .../target/adjust` sends the pilot's own +1, -1, +10, -10 key datagrams, so only those four steps
  exist. It is accepted in `auto`, `standby` and `route`. `PUT .../target` (absolute) checks `apData.mode`, which this
  provider never sets, so it always fails, and its heading path converts radians to degrees twice.
- **Target in wind:** `target/adjust` is **refused** in `wind` ("not in auto, standby or route mode"). The absolute `target`
  is broken as above, and the wind command behind it (`10 01 XX YY`) is the SeaTalk apparent wind **instrument** datagram, not
  a target. So the knob cannot change the wind angle through this provider today. On the pilot itself the +1 and +10 keys move
  the wind angle in vane mode, so the fix is upstream: allow the same key datagrams in `wind` **(verify on the pilot)**.
- **Tack:** `POST .../tack/port` and `.../tack/starboard` work in `auto` and `wind` (the -1-10 and +1+10 key pairs). Gybe,
  dodge, `courseNextPoint` and `courseCurrentPoint` are not implemented, so advancing a waypoint from the knob is not possible
  with this provider.
- **State and target come back** from `steering.autopilot.state` and `steering.autopilot.target.windAngleApparent` /
  `.headingMagnetic` on the server. The boat survey found **no `steering.*` paths**, so today the provider reports `state: null`
  whatever the pilot does. Without the pilot's status on the bus the knob cannot show or confirm any mode **(verify in the live
  control trials)**.
- Route on a SeaTalk 1 pilot (`86 11 03 FC`, "track") steers to navigation data that must reach the pilot on SeaTalk from a
  plotter or the server (APB/RMB or the SeaTalk equivalents). The course API on the server has no active course right now
  **(verify which device feeds the pilot)**.

Data the boat has today: `environment.wind.angleApparent` (rad, negative to port, NMEA 2000) and `.speedApparent`,
`environment.wind.directionTrue` from AdvancedWind, `navigation.headingTrue` and `.headingMagnetic`. The course API
(`/signalk/v2/api/vessels/self/navigation/course`) exists with `nextPoint`, `activeRoute`, `arrivalCircle`. The
calculated values (`navigation.course.calcValues.*`: distance, bearing, XTE) only appear with an active course **(verify the
path names when a route is active)**.

## Approach
- **Choosing the mode:** a long press while engaged opens a dialog like the anchor's: the states the pilot lists that are not
  the current one, then **Standby**, then **Cancel**. Changing state while engaged is a confirm press on the dialog line. From
  standby the existing long press plus confirm turn engages, and the engage prompt shows which mode it engages.
- **Engage goes through `PUT state`**, never `POST engage`, so the knob always engages the mode it showed (compass by default).
  This also changes compass mode today and belongs in the live control trials.
- **Wind screen:** apparent wind angle large, with `P` or `S`, target angle and the error, heading below. Turn changes the
  target by 1 or 10 degrees through `target/adjust`, once the provider accepts it in `wind`. Until then the turn is ignored and
  the screen says `ADJUST ON PILOT`. Tack, if offered: see open questions.
- **Route screen:** next waypoint name, bearing and distance, XTE with a left or right bar, heading. Turning does nothing.
- **Stale:** wind mode needs the wind angle and the pilot state, route mode needs the course values and the pilot state.
  Missing data shows the mode's stale overlay and blocks its controls, the ring and Standby stay available.
- **LEDs:** the steering indicator follows the wind angle error in wind and the XTE in route.
- **Config:** the visible pilot modes follow `options.states`, with a per-knob switch to hide wind or route.

## Test strategy
- Host tests in `knob_logic`: wind angle sign and P/S, angle error wrap at 180, XTE side and scale.
- Hardware checklist: each state change on the real pilot with live control on, engage from standby after a wind session
  engages compass, stale wind blocks the turn, Standby from every mode with one long press.
- Mockups of the wind, route and mode dialog screens agreed before coding.

## Open questions
- Does the pilot's state reach the server at all (no `steering.*` paths in the survey)? Without it nothing in this plan can be
  confirmed on screen. Part of the live control trials.
- Wind target: wait for an upstream change to `signalk-autopilot` (allow the key datagrams in `wind`), carry a local patch,
  or ship wind mode as display only with `ADJUST ON PILOT`?
- Tack from the knob: the provider offers it. Leave it out (issue default), or add it behind a dialog line plus confirm turn?
- Route: which device feeds the pilot its waypoint data, and is the server's course API the same route the pilot follows?
- Standby from wind or route: one long press like compass, or through the dialog?

## Steps
- [ ] Owner answers the open questions.
- [ ] Live control trials prove compass mode and show whether the pilot state reaches the server.
- [ ] Engage through `PUT state` with the shown mode (also fixes compass engage after a wind session).
- [ ] Upstream issue or PR for `target/adjust` in `wind` (and `setTarget` checking `state`), if chosen.
- [ ] Mockups: wind screen, route screen, mode dialog, engage prompt with the mode, both palettes.
- [ ] knob_model: wind, course and state subscriptions, snapshot fields. knob_actions: state, tack if chosen.
- [ ] marine_ui: two screens and the dialog. knob_power: indicator source per mode.
- [ ] SPEC 6.3 and 12, ui-design 3.1, hardware checklist, manual.

## Files
`firmware/components/knob_model/knob_model.c`, `firmware/components/knob_actions/knob_actions.c`,
`firmware/components/marine_ui/*`, `firmware/components/knob_logic/*`, `firmware/components/knob_power/*`,
`firmware/components/knob_config/*`, `test/host/test_knob_logic.c`, `SPEC.md`, `ARCHITECTURE.md`, `docs/ui-design.md`,
`docs/hardware-checklist.md`, `docs/manual.md`.
