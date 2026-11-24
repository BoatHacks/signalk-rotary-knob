/* SPDX-License-Identifier: Apache-2.0
 * LVGL 9.3 port for the CrowPanel 1.28" GC9A01 display. */
#pragma once

#include "lvgl.h"

/* Creates the display and the LVGL task. Needs board_display_init() to have run. */
void lv_port_init(void);

/* Diagnostics for stall hunting: who holds the lock and for how long, and how long the LVGL task has been inside
 * lv_timer_handler (0 when it is not). Cheap, safe to call from any task. */
typedef struct {
    const char *holder;          /* task name, "none" when free */
    uint32_t held_ms;            /* time since the current holder took the lock */
    uint32_t in_handler_ms;      /* time the LVGL task has spent in this lv_timer_handler call */
} lv_port_diag_t;
void lv_port_get_diag(lv_port_diag_t *d);

/* LVGL is not thread safe. Hold this lock around any lv_* or marine_ui_* call made outside the LVGL task. */
void lv_port_lock(void);
void lv_port_unlock(void);
