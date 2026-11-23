/* SPDX-License-Identifier: Apache-2.0
 * The knob's data model: the only owner of marine_ui_snapshot_t. Fills it from SignalK (stream subscriptions through espOS,
 * REST polls) and from knob_net, pushes it to marine_ui, and turns UI actions into SignalK calls (the adapters).
 * See ARCHITECTURE.md sections 2.5 and 2.6. */
#pragma once

#include <stdint.h>

#include "marine_ui.h"

/* Pass this to marine_ui_init(). It only queues the action. */
void knob_model_action_cb(const marine_ui_action_t *act, void *ctx);

/* Starts the model and worker tasks, so the UI shows the splash at once. Call after marine_ui_init(). */
void knob_model_start(void);

/* Subscribes to the SignalK paths. Call after espos_start(). */
void knob_model_subscribe(void);

/* Opens a mode from outside the mode ring (the REST API) and stores it as the last mode. mode is a marine_ui_mode_t, already
 * checked (knob_mode_request). Not under the LVGL lock. */
void knob_model_set_mode(int mode);

typedef struct {
    const char *link;   /* "online", "connecting", "setup", "pairing", "failed" or "down" (bench command x) */
    int alarm;          /* marine_alarm_type_t on screen */
    bool demo;          /* demo mode: simulated data, nothing sent */
} knob_model_status_t;
void knob_model_get_status(knob_model_status_t *out);

/* Whether a mode is on the mode ring by the settings (knob.volume_mode for VOLUME). The REST API refuses a hidden one. */
bool knob_model_mode_enabled(int mode);

/* Opens the mode stored at the last mode ring change (USB-HID excepted). Call after espos_start(), not under the LVGL lock. */
void knob_model_restore_mode(void);

/* Bench commands, one character each (from the USB serial port):
 *  a anchor drag alarm   g GPS lost alarm   c clear injected alarm   x toggle stream-down display
 *  s freeze the heading (goes stale)   n cycle night override (follow server, night, day)
 *  d bypass the network gating (show the operational UI without a stream)
 *  l flip allow control for this session only (bench override on top of the stored setting knob.allow_control)
 *  h help */
void knob_model_command(char c);

/* Stall diagnostics. phase: 1 building, 2 waiting for the LVGL lock, 3 inside marine_ui_apply_snapshot, 4 sleeping. */
void knob_model_get_diag(int *phase, uint32_t *loop_age_ms, uint32_t *apply_age_ms);
