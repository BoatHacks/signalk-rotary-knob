/* SPDX-License-Identifier: Apache-2.0
 * Demo mode simulation (#4): a boat for the UI to show when the knob runs without a server. Pure C, no ESP-IDF, host tested.
 * knob_model feeds its state into the snapshot and applies the UI's actions here instead of sending them anywhere. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNOB_DEMO_LIGHTS 6

typedef struct {
    uint32_t last_ms;          /* time of the last step */
    float heading_deg;         /* 0..360 */
    float target_deg;          /* autopilot target, follows the heading in standby */
    bool engaged;
    float rudder_deg;          /* + starboard */
    bool anchor_down;
    float radius_m;            /* watch circle */
    float anchor_dist_m;       /* boat from the anchor */
    bool dragging;             /* the drag alarm, latched (knob_demo_dragging) */
    float anchor_bearing_deg;  /* boat from the anchor */
    float swing_m;             /* how far the boat swings out from the anchor */
    float swing_target_m;      /* the swing moves towards it at 1 m a second (knob_demo_set_swing) */
    bool light[KNOB_DEMO_LIGHTS];
    uint8_t level[KNOB_DEMO_LIGHTS]; /* dimmer level while on, 0..100 (kept while off, restored when switched on) */
    float depth_m, voltage_v;
} knob_demo_t;

void knob_demo_init(knob_demo_t *d, uint32_t now_ms, uint16_t radius_m);
/* Moves the simulation to now_ms: the heading drifts in standby and follows the target when engaged (at most 3 degrees a
 * second, the rudder follows the error), the boat swings on the anchor (the bearing sweeps about 40 degrees either way over a
 * minute, the distance breathes around swing_m), depth and voltage wander a little. */
void knob_demo_step(knob_demo_t *d, uint32_t now_ms);

/* The UI's actions, applied at once. */
void knob_demo_engage(knob_demo_t *d, bool on);       /* engage keeps the current heading as the target */
void knob_demo_set_target(knob_demo_t *d, float deg);
void knob_demo_drop(knob_demo_t *d, uint16_t radius_m);
void knob_demo_raise(knob_demo_t *d);
void knob_demo_set_radius(knob_demo_t *d, uint16_t radius_m);
void knob_demo_set_light(knob_demo_t *d, int index, bool on);   /* on restores the kept level (100 if none) */
void knob_demo_set_level(knob_demo_t *d, int index, int pct);  /* 0 switches it off and keeps the level it had */
/* How far the boat swings out: it moves there at 1 m a second. The automatic demo (#10) widens it to raise the drag alarm. */
void knob_demo_set_swing(knob_demo_t *d, float m);

/* The drag alarm: raised when the boat leaves the watch circle, then kept although the distance breathes in and out, until
 * the radius is set to one the boat is inside of, or the anchor is moved or raised. */
bool knob_demo_dragging(const knob_demo_t *d);

#ifdef __cplusplus
}
#endif
