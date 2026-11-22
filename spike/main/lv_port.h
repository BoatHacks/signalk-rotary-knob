/* Spike: LVGL 9.3 port for the CrowPanel 1.28": GC9A01 display, encoder indev (rotation + push) and touch indev. */
#pragma once

#include "lvgl.h"

/* Creates the display, the encoder and touch input devices and the LVGL task. Needs board_display_init() and
 * board_input_init() to have run. */
void lv_port_init(void);

/* LVGL is not thread safe. Hold this lock around any lv_* call made outside the LVGL task. */
void lv_port_lock(void);
void lv_port_unlock(void);

/* The group the encoder drives. Add focusable widgets to it. */
lv_group_t *lv_port_group(void);
