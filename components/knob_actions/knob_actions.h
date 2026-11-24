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
 * Autopilot and anchor commands are only sent while allow control is on: the stored setting knob.allow_control, unless the
 * serial bench override is set for this session (SPEC section 2). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "knob_config.h"
#include "marine_ui.h"

/* What the adapters need to read from the model. */
typedef struct {
    int8_t light[8];        /* -1 unknown, 0 off, 1 on */
    char zone_id[4][24];    /* jukebox zone ids, target 1.. (empty = none) */
    bool ap_has_target;     /* the pilot's current target, for target/adjust */
    float ap_target_deg;
    bool zone_names_wanted; /* a zone id has no name yet */
    bool anchor_on;
    float anchor_max_m;     /* circle radius while armed */
    bool pos_valid;
    double lat, lon;
    uint32_t pos_ms;        /* knob time the position arrived */
} knob_actions_view_t;

/* What the adapters call back into the model for. */
typedef struct {
    void (*get_view)(knob_actions_view_t *out);
    void (*set_autopilot)(bool engaged, bool has_target, double target_rad, const char *state); /* from the status poll */
    void (*ack_latch)(void);          /* the alarm was acknowledged on the knob */
    bool (*alarm_injected)(void);     /* the alarm is a bench one, no server notification behind it */
    void (*action_result)(uint8_t code); /* what became of an anchor action, a marine_action_result_t */
    void (*autopilot_id)(char *out, size_t n); /* a copy of the current autopilot id (the setting may change at any time) */
    void (*zone_name)(const char *id, const char *name); /* a jukebox zone's name from the plugin's REST API */
} knob_actions_hooks_t;

/* Starts the worker task. cfg and hooks must outlive it. cfg is read for the light channels, which only change with a restart.
 * The autopilot id comes through the hook. */
void knob_actions_start(const knob_cfg_t *cfg, const knob_actions_hooks_t *hooks);

/* From the UI action callback: queues the action, never blocks. */
void knob_actions_submit(const marine_ui_action_t *act);

/* The stored allow control setting changed (or was loaded). source names where from, for the log: "config", "boot". */
void knob_actions_set_allow_control(bool on, const char *source);
/* Serial command l: flips allow control for this session only, as a bench override on top of the stored setting. */
void knob_actions_toggle_override(void);
/* Demo mode: every action bound for SignalK is dropped and the autopilot poll stops. USB-HID still works. */
void knob_actions_set_demo(bool on);
/* Whether autopilot and anchor commands are sent now (stored setting or override). */
bool knob_actions_is_live(void);

/* The anchor watch circle radius chosen on the knob, used by the next arm. */
uint16_t knob_actions_anchor_radius(void);
void knob_actions_set_anchor_radius(uint16_t metres);
