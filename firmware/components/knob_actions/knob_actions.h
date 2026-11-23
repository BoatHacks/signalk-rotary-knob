/* SPDX-License-Identifier: Apache-2.0
 * The adapters: turn UI actions into SignalK calls. One worker task takes the actions from a queue and may block on HTTP.
 * It also polls the autopilot status. All SignalK API details (paths, endpoints, bodies) live in knob_actions.c, see
 * ARCHITECTURE.md section 2.6.
 *
 *   lights       PUT 1 or 0 (relay) or a ratio (dimmer) on the channel's path, through the espOS stream
 *   autopilot    SignalK v2 autopilot API: engage, disengage, target
 *   anchor       hoekens-anchor-alarm REST: dropAnchor, setZone, raiseAnchor
 *   alarm ack    the server's Notifications API
 *
 * Autopilot and anchor commands are only sent while LIVE CONTROL is on (off at every boot). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "knob_config.h"
#include "marine_ui.h"

/* What the adapters need to read from the model. */
typedef struct {
    int8_t light[8];        /* -1 unknown, 0 off, 1 on */
    bool anchor_on;
    float anchor_max_m;     /* circle radius while armed */
    bool pos_valid;
    double lat, lon;
    uint32_t pos_ms;        /* knob time the position arrived */
} knob_actions_view_t;

/* What the adapters call back into the model for. */
typedef struct {
    void (*get_view)(knob_actions_view_t *out);
    void (*set_autopilot)(bool engaged, bool has_target, double target_rad); /* from the status poll */
    void (*ack_latch)(void);          /* the alarm was acknowledged on the knob */
    bool (*alarm_injected)(void);     /* the alarm is a bench one, no server notification behind it */
} knob_actions_hooks_t;

/* Starts the worker task. cfg and hooks must outlive it. The settings are read through cfg each time, so a later load into
 * the same struct takes effect. */
void knob_actions_start(const knob_cfg_t *cfg, const knob_actions_hooks_t *hooks);

/* From the UI action callback: queues the action, never blocks. */
void knob_actions_submit(const marine_ui_action_t *act);

void knob_actions_set_live(bool live);
bool knob_actions_is_live(void);

/* The anchor watch circle radius chosen on the knob, used by the next arm. */
uint16_t knob_actions_anchor_radius(void);
void knob_actions_set_anchor_radius(uint16_t metres);
