/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_demo.h"

#include <math.h>
#include <string.h>

#include "knob_logic.h"

#define DEMO_PI 3.14159265358979f

static float wrap360(float d)
{
    d = fmodf(d, 360.0f);
    return d < 0 ? d + 360.0f : d;
}

void knob_demo_init(knob_demo_t *d, uint32_t now_ms, uint16_t radius_m)
{
    memset(d, 0, sizeof(*d));
    d->last_ms = now_ms;
    d->heading_deg = 284.0f;
    d->target_deg = 284.0f;
    d->radius_m = radius_m ? radius_m : 35;
    d->swing_m = 22.0f;
    d->anchor_bearing_deg = 200.0f;
    d->depth_m = 6.4f;
    d->voltage_v = 12.8f;
}

void knob_demo_step(knob_demo_t *d, uint32_t now_ms)
{
    float dt = (float)(now_ms - d->last_ms) / 1000.0f;
    d->last_ms = now_ms;
    if (dt <= 0.0f) return;
    if (dt > 5.0f) dt = 5.0f; /* a long gap does not jump the boat */
    float t = (float)now_ms / 1000.0f;

    if (d->engaged) {
        float err = knob_course_error(d->target_deg, d->heading_deg);
        float turn = err;
        float max_turn = 3.0f * dt;
        if (turn > max_turn) turn = max_turn;
        if (turn < -max_turn) turn = -max_turn;
        d->heading_deg = wrap360(d->heading_deg + turn + 0.3f * sinf(t * 0.7f) * dt);
        d->rudder_deg = err * 2.0f;
        if (d->rudder_deg > 30.0f) d->rudder_deg = 30.0f;
        if (d->rudder_deg < -30.0f) d->rudder_deg = -30.0f;
    } else {
        d->heading_deg = wrap360(d->heading_deg + 0.8f * sinf(t * 0.05f) * dt);
        d->target_deg = d->heading_deg;
        d->rudder_deg = 0.0f;
    }

    if (d->anchor_down) {
        d->anchor_bearing_deg = wrap360(200.0f + 40.0f * sinf(t * 2.0f * DEMO_PI / 60.0f));
        d->anchor_dist_m = d->swing_m * (0.85f + 0.15f * sinf(t * 2.0f * DEMO_PI / 23.0f));
    }

    d->depth_m = 6.4f + 0.3f * sinf(t * 0.11f);
    d->voltage_v = 12.8f + 0.1f * sinf(t * 0.013f);
}

void knob_demo_engage(knob_demo_t *d, bool on)
{
    if (on && !d->engaged) d->target_deg = d->heading_deg;
    d->engaged = on;
}

void knob_demo_set_target(knob_demo_t *d, float deg) { d->target_deg = wrap360(deg); }

void knob_demo_drop(knob_demo_t *d, uint16_t radius_m)
{
    d->anchor_down = true;
    if (radius_m) d->radius_m = radius_m;
    d->anchor_dist_m = 0.0f;
}

void knob_demo_raise(knob_demo_t *d)
{
    d->anchor_down = false;
    d->anchor_dist_m = 0.0f;
}

void knob_demo_set_radius(knob_demo_t *d, uint16_t radius_m)
{
    if (radius_m) d->radius_m = radius_m;
}

void knob_demo_set_light(knob_demo_t *d, int index, bool on)
{
    if (index >= 0 && index < KNOB_DEMO_LIGHTS) d->light[index] = on;
}

bool knob_demo_dragging(const knob_demo_t *d)
{
    return d->anchor_down && d->anchor_dist_m > d->radius_m;
}
