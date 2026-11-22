/* SPDX-License-Identifier: Apache-2.0
 * Bring-up stand-in for knob_model: a simulated SignalK state that applies the UI actions locally, so marine_ui can be
 * exercised on the panel before the real model and adapters exist. Replaced by knob_model. */
#pragma once

#include "marine_ui.h"

/* Pass this to marine_ui_init(). It only queues the action. */
void demo_model_action_cb(const marine_ui_action_t *act, void *ctx);
void demo_model_start(void);
/* Stall diagnostics. phase: 1 actions, 2 waiting for the LVGL lock, 3 inside marine_ui_apply_snapshot, 4 sleeping. */
void demo_model_get_diag(int *phase, uint32_t *loop_age_ms, uint32_t *apply_age_ms);
/* Bench command from the CDC port: a = anchor drag alarm, g = GPS alarm, c = clear alarm, x = stream down,
 * s = freeze heading (stale), n = night palette, d = show the operational UI without waiting for the network,
 * h = help. */
void demo_model_command(char c);
