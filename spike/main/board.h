/* Spike: raw hardware bring-up for the Elecrow CrowPanel 1.28" rotary display (ESP32-S3).
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
void board_display_text_counter(int value, bool pressed); /* redraws the centre box */
void board_input_init(void);               /* PCNT encoder, button, touch probe */
int  board_encoder_count(void);            /* raw quadrature edges, 4 per detent step */
bool board_button_pressed(void);
bool board_touch_read(uint16_t *x, uint16_t *y); /* true while a finger is down */
bool board_touch_present(void);
