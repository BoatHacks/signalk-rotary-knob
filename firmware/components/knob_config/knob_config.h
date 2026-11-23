/* SPDX-License-Identifier: Apache-2.0
 * The knob's own settings, stored in the espOS config store (NVS) under the "knob" namespace and edited on the espOS settings
 * page or through its REST config API. Descriptor: config/knob.json. All keys apply after a restart. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define KNOB_CFG_MAX_LIGHTS 6

typedef struct {
    char name[16];
    char path[128];
    bool onoff;      /* true: a relay (PUT 1 or 0), false: a dimmer (PUT a ratio 0 to 1) */
} knob_cfg_light_t;

typedef struct {
    char autopilot_id[32];
    bool heading_true;       /* true: navigation.headingTrue, false: navigation.headingMagnetic */
    uint16_t anchor_radius_m;
    int n_lights;            /* channels with a non-empty path, compacted to the front of lights[] */
    knob_cfg_light_t lights[KNOB_CFG_MAX_LIGHTS];
} knob_cfg_t;

/* Reads the settings. Falls back to the descriptor defaults, so it always fills in something usable. */
void knob_config_load(knob_cfg_t *out);
