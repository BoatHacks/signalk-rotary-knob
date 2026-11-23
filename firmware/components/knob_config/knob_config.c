/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "espos_config.h"

static const char *TAG = "knob_config";
#define NS "knob"

void knob_config_load(knob_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->autopilot_id, "raymarineST");
    c->heading_true = true;
    c->anchor_radius_m = 35;

    espos_config_get_str(NS, "autopilot_id", c->autopilot_id, sizeof(c->autopilot_id), NULL);
    char hs[12] = "true";
    espos_config_get_str(NS, "heading_source", hs, sizeof(hs), NULL);
    c->heading_true = strcmp(hs, "magnetic") != 0;
    int32_t r = 35;
    if (espos_config_get_i32(NS, "anchor_radius", &r) == ESP_OK && r >= 5 && r <= 500) c->anchor_radius_m = (uint16_t)r;

    for (int i = 1; i <= KNOB_CFG_MAX_LIGHTS; i++) {
        char key[20], path[128] = "", name[16] = "", form[8] = "onoff";
        snprintf(key, sizeof(key), "light%d_path", i);
        espos_config_get_str(NS, key, path, sizeof(path), NULL);
        if (!path[0]) continue;
        snprintf(key, sizeof(key), "light%d_name", i);
        espos_config_get_str(NS, key, name, sizeof(name), NULL);
        snprintf(key, sizeof(key), "light%d_form", i);
        espos_config_get_str(NS, key, form, sizeof(form), NULL);
        knob_cfg_light_t *l = &c->lights[c->n_lights++];
        strncpy(l->path, path, sizeof(l->path) - 1);
        if (name[0]) strncpy(l->name, name, sizeof(l->name) - 1);
        else snprintf(l->name, sizeof(l->name), "LIGHT %d", i);
        for (char *p = l->name; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        l->onoff = strcmp(form, "level") != 0;
    }
    ESP_LOGI(TAG, "autopilot '%s', heading %s, anchor radius %u m, %d light channel(s)", c->autopilot_id,
             c->heading_true ? "true" : "magnetic", (unsigned)c->anchor_radius_m, c->n_lights);
}
