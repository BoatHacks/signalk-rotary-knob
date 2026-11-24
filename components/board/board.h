/* SPDX-License-Identifier: Apache-2.0
 * Pins from docs/hardware.md, checked against Elecrow's Arduino examples. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOARD_PIN_RAIL_1   1   /* gates LCD + touch power */
#define BOARD_PIN_RAIL_2   2   /* gates 5 V on the MX1.25 connectors */
#define BOARD_PIN_LCD_SCLK 10
#define BOARD_PIN_LCD_MOSI 11
#define BOARD_PIN_LCD_DC   3
#define BOARD_PIN_LCD_CS   9
#define BOARD_PIN_LCD_RST  14
#define BOARD_PIN_LCD_BL   46
#define BOARD_PIN_TP_SDA   6
#define BOARD_PIN_TP_SCL   7
#define BOARD_PIN_TP_INT   5
#define BOARD_PIN_TP_RST   13
#define BOARD_PIN_ENC_A    45
#define BOARD_PIN_ENC_B    42
#define BOARD_PIN_ENC_SW   41
#define BOARD_PIN_LED_DATA 48
#define BOARD_PIN_PWR_LED  40

#define BOARD_LCD_W 240
#define BOARD_LCD_H 240

void board_power_on(void);                 /* rails + power LED */
void board_display_init(void);             /* SPI + GC9A01 + backlight PWM, draws the test pattern */
void board_backlight_set(uint8_t percent); /* 0..100 */
void board_input_init(void);               /* PCNT encoder, button, touch probe */
int  board_encoder_count(void);            /* raw quadrature edges */

/* Quadrature edges per UI step. PCNT counts all four edges of a quadrature cycle, so 4 matches an encoder with one
 * detent per cycle. UNVERIFIED on the real encoder: if one detent gives 2 steps or 0.5 steps, change this. */
#ifndef BOARD_ENC_EDGES_PER_STEP
#define BOARD_ENC_EDGES_PER_STEP 4
#endif
/* Set to 1 if clockwise turns count down. UNVERIFIED: the expected direction is clockwise = positive. */
#ifndef BOARD_ENC_INVERT
#define BOARD_ENC_INVERT 0
#endif
int  board_encoder_take_steps(void);       /* whole steps since the last call, clockwise positive, remainder kept */

/* Asynchronous pixel push for a UI toolkit. (x2, y2) are exclusive. Call board_display_set_flush_cb() first; the
 * callback runs in ISR context when the transfer has finished. Before a callback is set, draws block instead. */
typedef void (*board_flush_done_cb_t)(void *ctx);
void board_display_set_flush_cb(board_flush_done_cb_t cb, void *ctx);
void board_display_draw(int x1, int y1, int x2, int y2, const void *rgb565_swapped);
/* Touch axes to screen axes. UNVERIFIED on the device: the display is mirrored left-right in the driver, the touch
 * controller may need the same flip. The raw values are logged on each touch ("touch raw x=.. y=.."). Apply in the
 * order swap, then flip. Raw range is assumed 0..239 on both axes. */
#ifndef BOARD_TOUCH_SWAP_XY
#define BOARD_TOUCH_SWAP_XY 0
#endif
#ifndef BOARD_TOUCH_FLIP_X
#define BOARD_TOUCH_FLIP_X 0
#endif
#ifndef BOARD_TOUCH_FLIP_Y
#define BOARD_TOUCH_FLIP_Y 0
#endif
#define BOARD_LED_COUNT 5
void board_led_init(void);
void board_led_set(int index, uint8_t r, uint8_t g, uint8_t b); /* buffered, call board_led_show() */
void board_led_show(void);
bool board_button_pressed(void);
/* true while a finger is down, with its position. Out-of-range ghost touches are dropped. Centre touches are reported: pushing
 * the knob presses the glass there, and marine_ui's dead zone decides what they may do (they only wake a dark screen). */
bool board_touch_read(uint16_t *x, uint16_t *y);
bool board_touch_present(void);
