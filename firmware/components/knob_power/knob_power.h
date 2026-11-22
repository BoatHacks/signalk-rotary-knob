/* SPDX-License-Identifier: Apache-2.0
 * Backlight and LED output. The idle timing lives in marine_ui, which emits MARINE_ACT_POWER_SET_BACKLIGHT and the alarm
 * LED actions. This component only carries them out. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { KNOB_LED_ALARM_OFF = 0, KNOB_LED_ALARM_FAST, KNOB_LED_ALARM_SLOW } knob_led_alarm_t;

void knob_power_init(void);                      /* starts the LED task, needs board_power_on() */
void knob_power_set_backlight(uint8_t percent);  /* 0, 10 or 100 from the UI, any 0..100 is accepted */
void knob_power_led_alarm(knob_led_alarm_t mode);/* fast red flash, slow red pulse, or off */
void knob_power_led_state(uint8_t r, uint8_t g, uint8_t b); /* steady state colour, 0,0,0 for off */
void knob_power_set_night(bool night);           /* night mode: state colours off, alarm unchanged */
