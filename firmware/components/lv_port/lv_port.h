/* SPDX-License-Identifier: Apache-2.0
 * LVGL 9.3 port for the CrowPanel 1.28" GC9A01 display. */
#pragma once

#include "lvgl.h"

/* Creates the display and the LVGL task. Needs board_display_init() to have run. */
void lv_port_init(void);

/* LVGL is not thread safe. Hold this lock around any lv_* or marine_ui_* call made outside the LVGL task. */
void lv_port_lock(void);
void lv_port_unlock(void);
