# Plan: autopilot and anchor trials with live control

## Overview
The autopilot (SignalK v2 API) and anchor (hoekens-anchor-alarm) adapters are written, and the anchor screen and dialogs were
reworked, but none of it has sent a real command. This plan is the session at the boat that proves it, with the owner present.

## Relevant SPEC / ARCHITECTURE sections
- SPEC section 2 (domain rules), 6.3 (SignalK interfaces), 13 (open questions 3 and 11, boat survey).
- ARCHITECTURE.md section 2.6 (adapters), section 9 (gate: autopilot GET with the token).
- docs/ui-design.md sections 3.1 and 3.2.

## Approach
Bench first, boat second. Every step is one action, checked on the knob screen, in the knob log and on the server, before the
next one.
1. Bench, live control off: every anchor dialog choice must show the red NOT SENT screen with "LIVE CONTROL IS OFF".
2. Bench, live control on, anchor: drop here, move drop point, set radius, raise. The plugin is configured and the boat is on
   the mooring, so a drop is harmless. Check the plugin's state in the admin UI after each.
3. Drag alarm: shrink the radius below the current distance, the plugin raises the alarm, the knob takes over the screen,
   acknowledge from the knob, check the notification is acknowledged on the server.
4. Autopilot, owner at the helm, motoring in open water: engage (long press plus confirm turn), target changes in 1 and 10
   degree steps, standby. Then pull the knob's power while engaged and check the pilot keeps its state.

## Test strategy
All of it is on docs/hardware-checklist.md (autopilot and anchor tables). Server side checks through the admin UI and the
SignalK REST API.

Before step 1, allow control (SPEC section 2, the stored setting that replaces the bench latch) is off, and the knob shows the
refusal screen for anchor and autopilot alike.

## Findings from the provider source (2026-11-23)
Read from `@signalk/signalk-autopilot` 2.6.0 on the boat server, details in issue-7-autopilot-modes.md:
- The provider id is still `raymarineST` and `GET .../autopilots/raymarineST` answers (read without a token).
- `POST .../engage` engages the **last engaged state**, which can be `wind` or `route`. The knob uses `engage` today, so compass
  engage is only safe if the pilot was last in `auto`. Engaging through `PUT .../state {"value":"auto"}` avoids it (#7 plan).
- State and target come from `steering.autopilot.*`, and the survey found none on the server, so the knob may see `state: null`
  whatever the pilot does. Check before step 4 that the pilot's status reaches the server.
- `target/adjust` converts radians to degrees and rounds down before matching +1, -1, +10, -10. Check that -1 and -10 arrive
  as such (rounding may turn -1 into -2, which the provider refuses).

## Open questions
- Does the v2 API answer `GET .../autopilots/raymarineST` with the knob's token (gate item)? Check before the boat session.
- Does the plugin's drag alarm reach `notifications.navigation.anchor`, and is that the id the knob acknowledges?

## Steps
- [ ] Fix the internal RAM dip first (internal-ram-dip.md).
- [ ] Bench: allow control off, all anchor choices and autopilot commands refused with the reason.
- [ ] Wire the SeaTalk 1 pilot to the server (in and out) and add a connection that consumes `seatalkOut`. Until then no
      autopilot command reaches the pilot.
- [ ] Bench: does the pilot's state reach `steering.autopilot.state` on the server.
- [ ] Bench: autopilot GET with the token (gate item).
- [ ] Boat: anchor drop, move, radius, raise.
- [ ] Boat: drag alarm and acknowledge.
- [ ] Boat: autopilot engage, target, standby, power loss.
- [ ] Record results in the hardware checklist, close SPEC questions that were answered.

## Files
Probably none, unless a test fails. Then `knob_actions.c`, `knob_model.c` or `marine_ui.c`.
