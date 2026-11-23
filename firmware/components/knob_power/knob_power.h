/* SPDX-License-Identifier: Apache-2.0
 * Backlight and LED output. The idle timing lives in marine_ui, which emits MARINE_ACT_POWER_SET_BACKLIGHT and the alarm
 * LED actions. This component carries them out and owns the steady LED colours (see knob_power_apply_snapshot). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "marine_ui.h"

typedef enum { KNOB_LED_ALARM_OFF = 0, KNOB_LED_ALARM_FAST, KNOB_LED_ALARM_SLOW } knob_led_alarm_t;

void knob_power_init(void);                      /* starts the LED task, needs board_power_on() */
void knob_power_set_backlight(uint8_t percent);  /* from the UI: 100 is the active level, below it the dim level, 0 off */
void knob_power_set_brightness(uint8_t percent); /* active brightness (settings), 10..100, what the UI's 100 means */
void knob_power_set_leds(bool on);               /* settings: false keeps every LED dark, the alarm flash included */
void knob_power_led_alarm(knob_led_alarm_t mode);/* fast red flash, slow red pulse, or off. Overrides the steady colour. */
void knob_power_led_state(uint8_t r, uint8_t g, uint8_t b); /* steady colour, 0,0,0 for off (raw, see apply_snapshot) */
void knob_power_set_night(bool night);           /* night mode: steady colours off, alarm patterns unchanged */

/* Steady LED colour from the state, call whenever the snapshot changes (about 10 Hz is fine).
 *  - Autopilot screen active, engaged or in standby: a steering indicator. Red = the boat has to turn to port,
 *    green = to starboard. On course is yellow when engaged and blue in standby. Course error is cts - heading,
 *    wrapped to -180..180 (negative means port). In standby cts is the set target.
 *    "On course" is within KNOB_LED_ON_COURSE_ENTER_DEG, and it stays on course until the error passes
 *    KNOB_LED_ON_COURSE_LEAVE_DEG, so the colour does not flicker at the edge.
 *  - Heading or target stale: dim amber when engaged, off in standby.
 *  - Autopilot engaged on another screen: dim green. Anchor armed: dim cyan. Otherwise off.
 *  - Night mode: every steady colour is off except the steering indicator on the autopilot screen. Red and green are
 *    shown as in the day. The on-course colour (yellow engaged, blue standby) is shown for KNOB_LED_NIGHT_FADE_HOLD_MS after the change to it,
 *    then fades linearly to off over KNOB_LED_NIGHT_FADE_MS. Alarm patterns still win. */
#ifndef KNOB_LED_NIGHT_FADE_HOLD_MS
#define KNOB_LED_NIGHT_FADE_HOLD_MS 2000
#endif
#ifndef KNOB_LED_NIGHT_FADE_MS
#define KNOB_LED_NIGHT_FADE_MS 1000
#endif
#ifndef KNOB_LED_ON_COURSE_ENTER_DEG
#define KNOB_LED_ON_COURSE_ENTER_DEG 2.0f
#endif
#ifndef KNOB_LED_ON_COURSE_LEAVE_DEG
#define KNOB_LED_ON_COURSE_LEAVE_DEG 3.0f
#endif
void knob_power_apply_snapshot(const marine_ui_snapshot_t *snap, marine_ui_mode_t active_mode);
