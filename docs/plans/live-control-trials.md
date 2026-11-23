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

## Open questions
- Is the autopilot provider id still `raymarineST`, and does the v2 API on this server answer `GET .../autopilots/<id>` with the
  knob's token (gate item)? Check before the boat session.
- Does the plugin's drag alarm reach `notifications.navigation.anchor`, and is that the id the knob acknowledges?

## Steps
- [ ] Fix the internal RAM dip first (internal-ram-dip.md).
- [ ] Bench: live control off, all anchor choices refused with the reason.
- [ ] Bench: autopilot GET with the token (gate item).
- [ ] Boat: anchor drop, move, radius, raise.
- [ ] Boat: drag alarm and acknowledge.
- [ ] Boat: autopilot engage, target, standby, power loss.
- [ ] Record results in the hardware checklist, close SPEC questions that were answered.

## Files
Probably none, unless a test fails. Then `knob_actions.c`, `knob_model.c` or `marine_ui.c`.
