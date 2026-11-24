# Plan: #8 motor throttle control

## Overview
A MOTOR mode for the boat's motor from the helm: arm it like engaging the autopilot, then turn right for more ahead and left for
more astern, with neutral at 12 o'clock and a stop on neutral between forward and reverse. The centre shows RPM, torque, power
and the gear. Safety comes first: the knob only ever sends a throttle, never takes over a running motor, and stops talking when
anything is unclear so that the motor side falls back to neutral by itself. Decided with the owner on 2026-11-24, defaults and safety gaps reviewed the same day.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 2 (domain rules, allow control), 6.2a (REST API), 6.3 (SignalK interfaces), 7 (UI), 9 (configuration), 12.
- ARCHITECTURE.md sections 2.5 (knob_model), 2.6 (adapters), 2.7 (knob_logic), 2.9 (modes).
- docs/ui-design.md sections 2 (mode ring, input vocabulary), 4 (stale rules, allow control), 6 (wake).

## Decisions (2026-11-24)
- **Mode ring:** MOTOR after AUTO: AUTO, MOTOR, LIGHTS, ANCHOR, MUSIC, USB, SETUP. Propeller icon, label `MOTOR`. Hidden by
  default (`knob.motor_mode` off), since most boats have no motor the knob may drive. The mode number is appended (6, the
  numbers are stored as the last mode), the ring order is its own table.
- **Gating:** needs allow control, like the autopilot and the anchor. Without it the screen is view only and arming shows
  `CONTROL IS OFF`. The REST API can switch to the mode but can never arm or set a throttle.
- **Arming:** long press while disarmed, then a confirm turn to the right within 5 s, with the countdown arc, like the autopilot's engage. The
  knob arms only while the motor reports neutral, otherwise `SET NEUTRAL FIRST`. It never takes over a running motor.
- **Command:** the knob sends only a throttle, a signed ratio from -1.0 (full astern) to +1.0 (full ahead), 0 is neutral. The
  motor controller reports F, N or R on its own path. The knob never sends a gear.
- **Turn:** right raises, left lowers. Steps depend on the turning speed, with three presets as a setting: Gentle 1/1/2 %,
  Normal 1/2/5 % (default), Quick 1/3/10 % (slow, normal and fast clicks). A click is fast under 50 ms after the last one,
  slow over 150 ms, normal between (spike 2026-11-24, docs/hardware.md "Encoder"). With 30 detents per turn a full turn at
  normal speed is 30, 60 or 90 %, a fast spin with Normal reaches full power in two thirds of a turn.
- **Neutral stop:** from ahead to astern or back the throttle stops at 0, further clicks are ignored. Once the knob has been
  still for about 1 s, the next turn goes on into the other direction. A fast spin never crosses neutral.
- **Presses while armed** (the classifier waits 300 ms after a press to tell single from double, so a single press is not
  fast enough for neutral and is left unused):
  - **Long press out of neutral:** throttle 0 at once when the hold reaches 750 ms (while still holding), stays armed.
  - **Long press in neutral:** disarm. From running, two long presses stop and disarm.
  - **Single press:** nothing.
  - **Double press:** acknowledges the limit warning while it shows, otherwise opens the mode ring as everywhere.
- **Limits:** two settings, max ahead and max astern (10 to 100 %), both 50 % by default. The first time in an arm cycle the
  throttle would pass a limit it stops there, nothing above the limit is sent, and a warning shows
  (`LIMIT 50 %`, `2x: ALLOW MORE`). A double press lifts both limits until the next disarm, the throttle stays where it is and
  only rises with the next turn. Turning back below the limit leaves the warning shown until acknowledged or disarmed.
  While the warning shows, all five LEDs flash amber fast (4 Hz, on and off equally), on every screen. Unlike the throttle
  pattern it also shows at night, at the night brightness, because it is a warning. The LEDs setting off turns it off, an
  alarm takes over the LEDs as always.
- **No ramp** in the knob, the motor controller does its own.
- **Keepalive:** while armed and out of neutral, the current throttle is resent every 1 s. Requirement on the motor side
  (whichever plugin is used or written): fall back to neutral after about 3 s without a throttle.
- **Link loss or stale motor data** while armed: send neutral if the link still allows it, disarm, show `NO LINK` or
  `NO MOTOR DATA`. Control only comes back with a new arm. Stale means the gear is older than 3 s. Only the gear is required,
  RPM, torque and power are optional; a boat that publishes no gear cannot arm.
- **Motor fault:** the gear reports `Fault`, or it does not match the throttle's sign (say +40 % but `Neutral`) for more than
  3 s: send 0, disarm, show `MOTOR FAULT` in red until the next arm. Arming is refused while the gear reports `Fault`.
- **Allow control switched off** (web page, API, settings) while armed: send 0 once and disarm.
- **Idle:** armed in neutral with no input, disarm after 60 s. The timeout is a setting, 10 to 300 s, 0 for never. Out of
  neutral it never disarms by itself.
- **Screen while running:** while the motor is out of neutral the dim and screen-off timers are paused, on every screen. Night
  brightness still applies.
- **Leaving the mode:** the ring stays usable and the throttle keeps its value. While the motor is out of neutral every screen
  except the alarm screen shows a small tag at the bottom, like the DEMO tag: `F 40 %` in green or `R 15 %` in amber. Hidden
  in neutral.
- **Reboot:** the knob starts disarmed and sends nothing, so the keepalive stops and the motor side goes to neutral. The motor
  screen is restored like any other mode, disarmed.
- **Screen:** RPM, torque, power and the gear (F, N, R) in the centre. An arc on the rim from 12 o'clock: clockwise in green
  for ahead, counter-clockwise in amber for astern. A thick arc for the set throttle, a thin marker for what the motor reports.
  Values the boat does not publish are left out, not shown as zero. The arc's full length is 100 %, the limits are marked on it
  until they are lifted.
- **LEDs:** ahead fills clockwise 1 o'clock then 3:30 (0 to 50 %, 50 to 100 %, fading in), astern counter-clockwise 11 then
  8:30. The 6 o'clock LED shows the gear: green F, amber R, off in N. Off at night and with the LEDs setting off. Alarms win.
  Fill and fade as in [issue-9-led-levels.md](issue-9-led-levels.md).

## SignalK paths (defaults, all settings)
SignalK has no standard path for a throttle command. Defaults, `<id>` is `knob.motor_id` (default `main`):
- Throttle command: `PUT propulsion.<id>.throttle` with `{"value": <ratio>}`. Reported throttle on the same path.
- Gear: `propulsion.<id>.transmission.gear` (`Forward`, `Neutral`, `Reverse`, per the SignalK spec).
- RPM: `propulsion.<id>.revolutions` (Hz, shown as rpm).
- Torque: `propulsion.<id>.engineTorque` (spec: ratio, shown as %).
- Power: `knob.motor_power`, empty by default, the value is hidden while it is empty (the draw may come from the motor or
  a battery monitor). Watts.

## Approach
- **knob_logic** (host tested): the arming and throttle state machine as pure code: `knob_motor_t` with arm, confirm, turn
  (click time in, step out by preset), the neutral stop with its 1 s pause, long press, double press, the limits and their
  warning, stale gear, fault and mismatch, allow control off, idle and keepalive due times. The firmware feeds it events and the clock, and sends what it returns. Safety rules live here so they are tested.
- **knob_model:** subscriptions for the gear, RPM, torque, reported throttle and the power path, a motor stale reason, the
  snapshot fields, the motor state for the tag.
- **knob_actions:** the throttle PUT, the keepalive timer, neutral on disarm. Allow control and the demo lock as for the pilot.
- **marine_ui:** the motor screen, the arm confirm, `SET NEUTRAL FIRST`, `NO LINK`, `NO MOTOR DATA`, the tag on the other
  screens, the propeller icon, the ring with seven slots (check that seven labels fit at r86).
- **knob_power:** the throttle LED pattern and the limit warning's amber flash.
- **knob_config:** `motor_mode` (off), `motor_id` (`main`), `motor_power` (empty), `motor_curve` (normal),
  `motor_idle_s` (60, 0 never), `motor_ahead` and `motor_astern` (50 %). Settings screen items for the curve, the idle
  timeout and the two limits.
- **Demo mode:** a simulated motor (RPM and torque following the throttle, the gear following the sign) so the screen can be
  photographed without a motor.

## Test strategy
- Host tests: steps per preset and click time, the neutral stop (a fast spin from +30 % stops at 0, a pause then goes astern),
  arming refused out of neutral, without allow control, without a gear or on `Fault`, long press out of and in neutral, single
  press ignored, the limit stop and warning (with its flash flag), double press lifting both limits without raising the throttle, limits back after a
  disarm, stale gear and link loss disarm, mismatch for 3 s and fault disarm, allow control off, idle disarm only in neutral and
  never with 0, keepalive due every 1 s only out of neutral, nothing sent after a reboot.
- Hardware checklist: a new motor section, first against a test plugin or the demo, then with the real motor at the boat.
- At the boat, before the motor is connected: check that the motor side falls back to neutral without the keepalive.

## Open questions
- Which motor and which plugin take the throttle. The knob's defaults are a guess until then **(verify)**.

## Steps
- [x] **Spike `spikes/encoder-timing`** (2026-11-24): 30 detents per turn, 4 edges each, thresholds 50 and 150 ms.
- [x] knob_logic: `knob_motor` state machine and step presets, host tests (def719c). Clicks in the first 500 ms after arming
      are ignored, so a confirming spin cannot run the throttle up.
- [x] knob_config keys and settings items (62294f1).
- [x] knob_model: subscriptions, the state machine wired to inputs and the tick, the throttle as a PUT over the stream (no
      worker queue, a click wakes the model task so neutral goes out at once), demo motor (62294f1).
- [x] marine_ui: motor screen, arming, refusals, limit warning, tag, ring slot, keep-awake (ef12138).
- [x] knob_power: the throttle LED pattern (two LEDs per side with `knob_led_fill`, the bottom LED the reported gear, off at
      night) and the limit warning's amber 4 Hz flash on every screen, also at night.
- [x] Docs: SPEC 6.3, manual, ui-design (ring order, section 3.7), hardware checklist (motor section), ARCHITECTURE 2.9e.
- [x] Host tests run 2026-11-24 on the Pi: 37 tests, 0 failures, no warnings.
- [ ] Bench check in demo mode (the build on the knob since f9ac9d3 has the screen; the LEDs came with d6c03bb).
- [ ] The motor side: which plugin takes the throttle, then the checks at the boat.

## Files
- `spikes/encoder-timing/` (new)
- `components/knob_logic/knob_logic.{c,h}`, `test/host/test_knob_logic.c`
- `components/knob_config/` (`knob_config.{c,h}`, `config/knob.json`)
- `components/knob_model/knob_model.{c,h}`, `components/knob_actions/knob_actions.{c,h}`, `components/knob_logic/knob_demo.c`
- `components/marine_ui/marine_ui.c`, `components/marine_ui/include/marine_ui.h`
- `components/knob_power/knob_power.c`
- SPEC.md, ARCHITECTURE.md, docs/ui-design.md, docs/manual.md, docs/hardware-checklist.md
