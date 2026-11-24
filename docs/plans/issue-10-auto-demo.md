# Plan: #10 automatic demo mode

## Overview
A hands-free demo: the knob drives itself through every mode the way a person would, using the mode ring, and runs one typical
action in each, with every simulated turn, press and touch drawn on the screen. For trade shows, videos and the README. It builds
on the existing demo mode (#4): simulated boat, nothing sent anywhere.

## Relevant SPEC / ARCHITECTURE sections
- docs/plans/issue-4-demo-mode.md and ui-design section 4 "Demo mode": the simulation and the DEMO tag this builds on.
- ui-design section 2 (input vocabulary, mode ring), 3.1 to 3.6 (each mode's controls), 5 (alarm), 7 (LEDs).
- ARCHITECTURE section 2 (UI contract: raw inputs in, actions out).

## The script (owner, 2026-11-24)
Every step goes through the ring switcher and the mode's own controls, never a shortcut.

1. **Autopilot.** Standby. Long press, confirm turn right: engaged. Target 23 degrees to port: press (step 10), two clicks left,
   press (step 1), three clicks left. Wait until the heading has settled on the new target.
2. **Ring to Lights.** A light at 80 %: turn down to 20 % (12 clicks of 5 %), then long press: off.
3. **Ring to Anchor.** Raised. Radius 35 to 20 m (3 clicks left), press to apply. Long press, `DROP HERE`, press: spinner,
   then dropped.
4. **Ring to Music.** Playing `Robotti humppa` by `Eläkeläiset` at 0:53, the clock running. Volume 42 % to 100 % (29 clicks of 2 %).
5. **The drag alarm fires** (the boat swings past 20 m): full-screen takeover. Press: acknowledged, back to Music with the red
   ring. **Ring to Anchor.** Radius to 30 m (2 clicks right), press to apply: `SETTING RADIUS...`, then the alarm clears.
6. **Ring to USB.** Profile cursor keys to media: two long presses (the order is chartplotter, media, cursor keys).
7. **Ring to Setup.** Scroll to `DEMO MODE`, press, turn to off, press to store.

## Approach

### Script player
- A pure C step player in `knob_logic` (`knob_autodemo.c`, host tested). A step is one input (turn n clicks with a gap,
  press, long press, double press, touch at x,y) or a wait (a time, or a condition: the heading within 1 degree of the target,
  the anchor confirmed, the alarm on or off). Waits have a timeout, so a missed condition skips ahead instead of hanging.
- The player feeds the same entry points as the real hardware (`marine_ui_feed_encoder_step`, `marine_ui_feed_button_edge`,
  `marine_ui_feed_touch`) under the LVGL lock, so the real gesture timing applies: the 300 ms single press, the double press,
  the long press threshold. Nothing in the UI knows it is scripted, which also makes the demo a test of the real input path.
- Paced for a viewer: about 250 ms between clicks, a pause of 1.5 to 3 s after each result, a caption before each step.
- Runs in the input task, which already owns the feeds. Steps come from a const table, so the script is easy to change.

### Simulation additions (knob_demo)
- Dimmer levels per light (today on/off only), starting at 80 %.
- Music: master volume, play state, track, artist and a running position (`0:53` at the start), applied like the real
  jukebox values. Zones optional.
- Scripted drag: the swing grows past the radius while the script is in Music, then stays at about 25 m, so 20 m drags and
  30 m clears. Today the swing is fixed.
- USB host simulated as present, so the HID slot shows on the ring. In demo mode no HID report is sent and TinyUSB is not
  started: the HID screen and the profile change are UI only.
- The autopilot turn: the heading follows at 3 degrees a second already, about 8 s for 23 degrees.

### Visible inputs
An overlay on LVGL's top layer, above every screen and the alarm takeover, drawn only in the automatic demo:
- **Turn:** a curved arrow on the rim in the turning direction, with the click count (`-3`, `+29`), fading out 0.5 s after the
  last click.
- **Press:** a ring pulsing out from the centre. **Double press:** two quick pulses. **Long press:** a ring filling around the
  centre for the hold time, then a pulse.
- **Touch:** a fingertip circle at the touch point with a ripple.
- **Caption:** one short line in a pill at the top edge saying what happens (`ENGAGE`, `23° TO PORT`, `DIM TO 20 %`, `ACKNOWLEDGE`).
  Owner to confirm that captions are wanted.
- The DEMO tag stays. The LEDs do what the real UI does (alarm red, steering indicator), plus #9's levels once that exists.

### Fonts
The built-in Montserrat fonts cover ASCII, ° and • only, so `Eläkeläiset` shows boxes. Generated 2026-11-24 at 14, 20 and 32 px:
Montserrat with Latin-1, Latin Extended-A, Cyrillic and typographic punctuation, Greek from DejaVu Sans, LVGL's symbols.
Measured in a compile with the knob's flags: 43.5, 61.2 and 107.4 KB of flash (compressed format), no RAM. That also fixes
real track names.

### Starting, stopping, looping
- `DEMO MODE` becomes a choice: `OFF`, `ON` (today's manual demo), `AUTO`. Same key, same restart rule.
- Any real input during the automatic demo pauses the script and hands the knob to the person. After 60 s without input the
  script starts again from the top, so a trade-show knob that someone played with goes back to the demo by itself.
- The last step: in the automatic demo, storing `OFF` is shown but not applied, and the script starts again after a pause.
  A real `OFF` would end the demo and restart the knob. To end the automatic demo for real, a person changes the setting
  (their input pauses the script first).

## Test strategy
- Host tests: the step player (timing, waits and their timeouts, pause on real input, restart after idle), the music
  position clock, the scripted swing, dimmer levels in the simulation.
- Bench: the whole script runs to the end three times in a row without a stuck step, at day and at night. Every input is
  visible. Real input pauses it, 60 s later it starts again. Nothing on the network while it runs (as for #4).
  A new section "Automatic demo" in docs/hardware-checklist.md.
- Record one run on video for the README.

## Open questions
Taken as proposed on 2026-11-24 (owner: "start the full-auto demo plan", no answers given, so the plan's proposals stand;
easy to change in the script table):
- **Last step:** `OFF` is shown, not applied, and the tour loops.
- **Captions:** on, short upper-case words in a pill at the top edge.
- **Loop timing:** 60 s idle before the tour starts again, about two and a half minutes for one run.
- **Music position on the real knob** (still to verify): does signalk-jukebox publish the playback position? Until then the
  position line shows in demo mode only (`track_pos_s` is -1 otherwise).

Added by the owner on 2026-11-24: the tour opens with a 3, 2, 1 countdown, then the splash with a simulated boot.

## Steps
- [x] Owner questions: defaults taken, see above.
- [x] Fonts generated 2026-11-24 (owner chose the full set): `knob_font_14`, `_20`, `_32` in `components/marine_ui/fonts`,
      from `tools/gen_fonts.sh`. Measured flash cost, compressed: 43.5, 61.2 and 107.4 KB.
- [x] Track and artist on the music screen use `knob_font_14`. Zone and channel names still to switch.
- [x] knob_demo: dimmer levels, a swing that moves to a target. The music position, the simulated USB host and the
      three demo light channels live in knob_model. Host tests for the player.
- [x] Music screen: the position line (`0:53`, `PAUSED 0:53`) when the snapshot has one.
- [x] `knob_autodemo` step player and the script table in knob_logic, 5 host tests (gestures, waits and the countdown, ring
      and settings navigation, pause and restart, the whole script loops).
- [x] Input overlay in marine_ui (turn arc and count, press rings, long press fill, touch dot, caption, countdown).
- [x] Wired up: `DEMO MODE` `OFF`/`ON`/`AUTO` (`knob.demo` plus `knob.demo_auto`), the player in the input task's hook,
      pause on real input and restart after 60 s, serial command `p`, no USB device, no flash writes, the simulated boot.
- [x] First run on the knob 2026-11-24: crashed at the first caption, because the knob's fonts are compressed and
      `CONFIG_LV_USE_FONT_COMPRESSED` was off (fixed). Then one serial-started run over 3.5 minutes: no crash, the last step
      reached after about 108 s, the tour looped. Internal RAM dipped once to 22.4 KB around the engage step (normal low
      39.8 KB, espOS restarts below 12 KB): find what allocates there (the countdown's scaled label, the overlay) and bring it back.
- [ ] Bench run through the hardware checklist section "Automatic demo" by eye, video for the README.
- [x] Docs: ui-design section 4 "Automatic demo", manual section 6a, hardware checklist.

## Files
`components/knob_logic/knob_autodemo.{c,h}` (new), `components/knob_logic/knob_demo.{c,h}`, `components/input/input.c`,
`components/marine_ui/marine_ui.c` (overlay, position line, fonts), new `components/marine_ui/font_*_latin1.c`,
`components/knob_model/knob_model.c`, `components/knob_config/*`, `components/knob_usb/*`, `test/host/test_knob_logic.c`,
`docs/ui-design.md`, `docs/manual.md`, `docs/hardware-checklist.md`.
