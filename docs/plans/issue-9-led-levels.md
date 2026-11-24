# Plan: #9 LED levels for the music and lights modes

## Overview
The five WS2812s on the rim of the base become a level display in the music and lights modes: the volume or the light's
brightness fills the ring clockwise from the bottom left, and the bottom LED shows play or pause (music) or on or off (lights).
The same fill shows the throttle in the motor mode (#8). Decided with the owner on 2026-11-24, defaults reviewed the same day.

## Relevant SPEC / ARCHITECTURE sections
- SPEC sections 7 (UI), 9 (configuration, the LEDs and night settings), 12.
- ARCHITECTURE.md section 2.9 (knob_power, the LED task).
- docs/ui-design.md sections 3.3 (lights), 3.6 (music), and the LED rules. docs/hardware.md "Ambient LED positions".

## Decisions (2026-11-24)
- **Level:** four LEDs, a quarter of the range each, filled clockwise from the bottom left: 8:30 (index 3), 11 (index 2),
  1 o'clock (index 1), 3:30 (index 0). Each fades in smoothly over its quarter: at 50 % the left two are full and the right
  two off, from 50 % on the 1 o'clock LED slowly fades in. No pulsing.
- **6 o'clock LED (index 4):** music, green while playing, off when paused or stopped. Lights, warm white while the selected
  channel is on, off when it is off.
- **Colours (day):** music level violet-blue, lights level warm white.
- **On/off light channels** (relays): all five LEDs warm white while on, all off when off.
- **Night mode:** these patterns are off. The alarm and the autopilot's steering indicator stay as they are.
- **The LEDs setting** off turns these patterns off too.
- **Alarms** take over the LEDs as today, on every screen.
- **Stale data** (`ZONE OFFLINE`, a light without a value): all off, never a guessed level.
- **Value shown:** the level the server reports, not the value pending while turning, so the ring never shows a change that
  did not happen (it follows the hand with a short lag).
- **Muted zone:** the level at a quarter of its brightness, the 6 o'clock LED still shows play or pause.
- **Brightness:** scaled by the brightness setting and dimmed with the screen's idle dim, like the steering indicator.

## Approach
- **knob_logic** (host tested): `knob_led_fill(ratio, n, out)`, the brightness of n LEDs in fill order for a level from 0 to 1,
  used by music and lights (n = 4) and the motor's two sides (#8, n = 2). knob_power maps fill order to the data line.
- **knob_power:** a per-mode LED source next to the steering indicator: level and bottom LED from the snapshot, the colour per
  mode, scaled by the brightness setting, off at night and with the LEDs setting off. The LED task already sends only on a
  change (2026-11-24), so a turn updates the ring without flicker.
- Brightness follows the selected light group in lights mode, the selected target (master or zone) in music mode.

## Test strategy
- Host tests: `knob_led_fill` at 0, 12.5, 25, 50, 62.5 and 100 %, clamping outside 0 to 1, the motor's two-LED use.
- Hardware checklist: music level and play/pause, lights dimmer level and on/off, on/off channel all five, night mode off,
  LEDs setting off, alarm over a level, no flicker while turning.

## Open questions
None. The exact violet-blue and warm white values are picked on the panel during the hardware check.

## Steps
- [x] knob_logic: `knob_led_fill` (the fill for any number of LEDs, so the motor reuses it), host tests.
- [x] knob_power: music and lights level patterns, colours, night, LEDs setting, brightness and idle dim. The zone's `muted`
      is parsed and in the snapshot, marine_ui reports the selected channel and target.
- [x] Docs: ui-design section 7, manual (lights, music), hardware checklist.
- [ ] Build, flash, the hardware checklist rows (colours picked on the panel).

## Files
- `components/knob_logic/knob_logic.{c,h}`, `test/host/test_knob_logic.c`
- `components/knob_power/knob_power.c`
- docs/ui-design.md, docs/manual.md, docs/hardware-checklist.md
