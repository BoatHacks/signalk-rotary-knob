# Plan: #7 wind and route modes for the autopilot

## Overview
Add the pilot's wind (vane) and route modes to the autopilot screen, next to compass steering (`auto`), so the helm can steer
to the apparent wind or follow a route from the knob, and tack from the knob. Same safety rules as compass mode.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (domain rules, allow control), 6.3 (autopilot v2 API), 12 and 13.
- ARCHITECTURE.md sections 2.5 (knob_model), 2.6 (knob_actions and adapters), 3.1 (path map).
- docs/ui-design.md sections 3.1 (autopilot), 4 (stale rules), 7 (LEDs).
- docs/plans/live-control-trials.md: compass mode is proven on the boat first.

## The boat today (2026-11-23)
- The pilot is a **Raymarine SeaTalk 1** unit and is **not wired to the SignalK server yet** (owner). The server's
  `signalk-autopilot` plugin is set to `raymarineST` with output event `seatalkOut`, but no data connection consumes that event,
  so its datagrams go nowhere. That is also why the survey found no `steering.*` paths.
- No NMEA 0183 output plugin is installed that could send the route (APB, RMB) to the pilot. `course-provider` is enabled and
  computes the course values on the server.
- `environment.wind.angleApparent` (rad, negative to port) and `.speedApparent` come from NMEA 2000.

## What the provider does
Read from `@signalk/signalk-autopilot` 2.6.0 on the boat server and its live v2 API:
- The pilot's modes are API **states**: `standby`, `auto`, `wind`, `route`. `GET` and `PUT .../mode` answer "Not implemented",
  `options.modes` and `options.actions` are empty.
- **Engage** (`POST .../engage`) resumes the **last engaged state**, so after a wind or route session it engages wind or route.
  Selecting a state is `PUT .../state {"value":"wind"}`.
- **Target in auto:** `PUT .../target/adjust` sends the pilot's own +1, -1, +10, -10 key datagrams. Accepted in `auto`,
  `standby` and `route`. The absolute `PUT .../target` checks `apData.mode`, which this provider never sets, so it always fails.
- **Target in wind:** `target/adjust` is refused in `wind`. The absolute target is broken as above, and the wind command behind
  it (`10 01 XX YY`) is the apparent wind **instrument** datagram, not a target. On the pilot head the +1 and +10 keys move the
  wind angle in vane mode, so the fix is to allow the same key datagrams in `wind` **(verify on the pilot)**.
- **Tack:** `POST .../tack/port` and `.../tack/starboard` in `auto` and `wind` (the -1-10 and +1+10 key pairs). Gybe, dodge,
  `courseNextPoint` and `courseCurrentPoint` are not implemented, so the knob cannot advance a waypoint.
- State and target come back from `steering.autopilot.state` and `steering.autopilot.target.*`, which need the pilot's own
  SeaTalk output read in by the server.

## Decisions (2026-11-23)
- **Wind target:** an upstream PR to `signalk-autopilot` (allow the key datagrams in `wind`, make `setTarget` check `state`).
  Until it is released, wind mode shows the angle and the screen says `ADJUST ON PILOT`, the turn does nothing.
- **Tack from the knob:** yes, from the dialog plus a confirm turn, like engage.
- **Route:** the server's course API is what the pilot follows, through an NMEA 0183 output to SeaTalk that is still to be set
  up **(verify once wired)**. The knob shows the server's course values.
- **Standby while engaged:** a long press opens the pilot dialog with **STANDBY** first and focused, so long press and one press
  is standby. This replaces the direct long-press standby in compass mode too.

## Approach
- **Dialog** (long press while engaged): `STANDBY` (focused), the other modes the pilot lists (`COMPASS`, `WIND`, `ROUTE`),
  `TACK PORT` and `TACK STARBOARD` (in compass and wind only), `CANCEL`. Turn chooses, one press confirms. A mode change is
  that press. A tack line asks for the confirm turn (one click towards the tack side within 5 s) before it is sent. A long press
  or 5 s without input leaves the dialog.
- **Engage from standby:** long press plus confirm turn as now, but sent as `PUT .../state` with the mode shown in the prompt
  (always compass, `auto`), never `POST .../engage`. Wind or route is chosen from the dialog once engaged.
- **Wind screen:** apparent wind angle large with `P` or `S`, target angle and the error, heading below. Turn adjusts the target
  by 1 or 10 degrees through `target/adjust` once the provider supports it.
- **Route screen:** next waypoint name, bearing and distance, XTE with a left or right bar, heading. Turning does nothing.
- **Stale:** wind mode needs the wind angle and the pilot state, route mode needs the course values and the pilot state.
  Missing data shows the mode's stale overlay and blocks its controls, the ring and the dialog's STANDBY stay available.
- **LEDs:** the steering indicator follows the wind angle error in wind and the XTE in route.
- **Allow control:** every pilot command, tack included, needs it (refusal screen otherwise).

## Test strategy
- Host tests in `knob_logic`: wind angle sign and P/S, angle error wrap at 180, XTE side and scale, the dialog's line list per
  state, the tack confirm direction.
- Hardware checklist: each state change on the real pilot, engage after a wind session engages compass, tack both ways with the
  confirm turn, stale wind blocks the turn, standby from every mode with long press plus press.
- Mockups of the wind screen, the route screen and the dialog agreed before coding.

## Open questions
None for the plan. Facts to verify once the pilot is wired: the pilot's state and target on `steering.autopilot.*`, the wind
key behaviour, and the course output reaching the pilot.

## Steps
- [ ] Wire the pilot to the server: SeaTalk in and out, a connection that consumes `seatalkOut` (owner, with the live control
      trials).
- [ ] Set up the course output to the pilot (NMEA 0183 APB/RMB to SeaTalk) and check route mode on the pilot head.
- [ ] Live control trials prove compass mode.
- [x] Engage through `PUT state auto` (also fixes compass engage after a wind session). Not built yet.
- [x] knob_logic: states, dialog lines, wind side, XTE steer, tack confirm direction, with host tests (27 pass).
- [x] Contract (`ap_state`, wind and route fields, `MARINE_ACT_AP_SET_STATE` and `_TACK`), knob_actions (`PUT state`, tack),
      knob_model (state from the poll, wind and course subscriptions, snapshot, demo). The waypoint shows as `WAYPOINT` until a
      name source is found (verify).
- [x] marine_ui: pilot dialog (long press while engaged, STANDBY reachable while stale), tack confirm, wind and route variants of
      the autopilot screen, `NO WIND` / `NO ROUTE`. knob_power: indicator per mode.
- [x] Target changes as `target/adjust` steps (the absolute `PUT target` never works with this provider), with a host test.
- [x] Docs: SPEC 6.3, ui-design 3.1, manual, hardware checklist.
- [ ] Upstream issue and PR for `target/adjust` in `wind` and `setTarget` checking `state`.
- [x] Mockups: wind screen, route screen, dialog (agreed 2026-11-23, ui-design.md section 3.1). Night palette at build time.
- [x] knob_model: wind, course and state subscriptions, snapshot fields. knob_actions: state, tack.
- [x] marine_ui: two screens and the dialog. knob_power: indicator source per mode.
- [x] SPEC 6.3, ui-design 3.1, hardware checklist, manual.

## Files
`components/knob_model/knob_model.c`, `components/knob_actions/knob_actions.c`,
`components/marine_ui/*`, `components/knob_logic/*`, `components/knob_power/*`,
`components/knob_config/*`, `test/host/test_knob_logic.c`, `SPEC.md`, `ARCHITECTURE.md`, `docs/ui-design.md`,
`docs/hardware-checklist.md`, `docs/manual.md`.
