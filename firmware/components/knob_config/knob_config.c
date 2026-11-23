/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "espos_config.h"
#include "nvs.h"

static const char *TAG = "knob_config";
#define NS "knob"

void knob_config_load(knob_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strcpy(c->autopilot_id, "raymarineST");
    c->heading_true = true;
    c->anchor_radius_m = 35;
    c->dim_after_ms = 30000;
    c->dim_level_pct = 10;
    c->off_after_ms = 600000;

    espos_config_get_str(NS, "autopilot_id", c->autopilot_id, sizeof(c->autopilot_id), NULL);
    char hs[12] = "true";
    espos_config_get_str(NS, "heading_source", hs, sizeof(hs), NULL);
    c->heading_true = strcmp(hs, "magnetic") != 0;
    int32_t r = 35;
    if (espos_config_get_i32(NS, "anchor_radius", &r) == ESP_OK && r >= 5 && r <= 500) c->anchor_radius_m = (uint16_t)r;

    int32_t v = 0;
    if (espos_config_get_i32(NS, "dim_after_s", &v) == ESP_OK && v >= 5 && v <= 3600) c->dim_after_ms = (uint32_t)v * 1000u;
    if (espos_config_get_i32(NS, "dim_level_pct", &v) == ESP_OK && v >= 1 && v <= 100) c->dim_level_pct = (uint8_t)v;
    if (espos_config_get_i32(NS, "off_after_min", &v) == ESP_OK && v >= 0 && v <= 240) c->off_after_ms = (uint32_t)v * 60000u;

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

#define STATE_NS "knob_state"
static bool s_state_read;
static int s_last_mode = -1;
static bool s_last_hid;

static void state_read(void)
{
    if (s_state_read) return;
    s_state_read = true;
    nvs_handle_t h;
    if (nvs_open(STATE_NS, NVS_READONLY, &h) != ESP_OK) return;
    int8_t v;
    uint8_t b;
    if (nvs_get_i8(h, "mode", &v) == ESP_OK) s_last_mode = v;
    if (nvs_get_u8(h, "hid", &b) == ESP_OK) s_last_hid = b != 0;
    nvs_close(h);
}

int knob_config_last_mode(void)
{
    state_read();
    return s_last_mode;
}

bool knob_config_last_hid(void)
{
    state_read();
    return s_last_hid;
}

void knob_config_save_last_mode(int mode, bool hid)
{
    state_read();
    if (hid) mode = s_last_mode;
    if (mode == s_last_mode && hid == s_last_hid) return;
    nvs_handle_t h;
    esp_err_t e = nvs_open(STATE_NS, NVS_READWRITE, &h);
    if (e == ESP_OK) {
        e = nvs_set_i8(h, "mode", (int8_t)mode);
        if (e == ESP_OK) e = nvs_set_u8(h, "hid", hid ? 1 : 0);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
    }
    if (e == ESP_OK) {
        s_last_mode = mode;
        s_last_hid = hid;
    } else {
        ESP_LOGW(TAG, "last mode not stored: %s", esp_err_to_name(e));
    }
}
