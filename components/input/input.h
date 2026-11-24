/* SPDX-License-Identifier: Apache-2.0
 * Input task: reads the encoder, button and touch from board and feeds the marine_ui semantic classifier. Also drives
 * marine_ui_tick(). All marine_ui calls are made under the LVGL port lock. */
#pragma once

void input_init(void);
