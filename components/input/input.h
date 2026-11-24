/* SPDX-License-Identifier: Apache-2.0
 * Input task: reads the encoder, button and touch from board and feeds the marine_ui semantic classifier. Also drives
 * marine_ui_tick(). All marine_ui calls are made under the LVGL port lock. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void input_init(void);

/* The automatic demo (#10): while enabled, the hook runs every input period (5 ms) under the LVGL port lock, after the real
 * inputs were fed. user is true when there was real input in this period (a click, a button edge or a touch). */
typedef void (*input_hook_t)(uint32_t now_ms, bool user);
void input_set_hook(input_hook_t hook);
void input_hook_enable(bool on);