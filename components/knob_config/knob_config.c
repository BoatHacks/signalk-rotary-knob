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
    c->brightness_pct = 100;
    c->leds_on = true;
    c->music_mode = true;

    bool allow = false;
    if (espos_config_get_bool(NS, "allow_control", &allow) == ESP_OK) c->allow_control = allow;
    bool demo = false;
    if (espos_config_get_bool(NS, "demo", &demo) == ESP_OK) c->demo = demo;
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
    if (espos_config_get_i32(NS, "brightness_pct", &v) == ESP_OK && v >= 10 && v <= 100) c->brightness_pct = (uint8_t)v;
    char nm[8] = "server";
    espos_config_get_str(NS, "night_mode", nm, sizeof(nm), NULL);
    c->night_mode = !strcmp(nm, "day") ? 1 : !strcmp(nm, "night") ? 2 : 0;
    bool leds = true;
    if (espos_config_get_bool(NS, "leds_on", &leds) == ESP_OK) c->leds_on = leds;
    bool vm = true;
    if (espos_config_get_bool(NS, "music_mode", &vm) == ESP_OK) c->music_mode = vm;
    char hp[16] = "chartplotter";
    espos_config_get_str(NS, "hid_profile", hp, sizeof(hp), NULL);
    c->hid_profile = !strcmp(hp, "media") ? 1 : !strcmp(hp, "nav") ? 2 : 0;

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
    ESP_LOGI(TAG, "autopilot '%s', heading %s, anchor radius %u m, %d light channel(s), allow control %s", c->autopilot_id,
             c->heading_true ? "true" : "magnetic", (unsigned)c->anchor_radius_m, c->n_lights, c->allow_control ? "on" : "off");
}

static bool set_done(const char *key, esp_err_t e)
{
    if (e != ESP_OK) ESP_LOGW(TAG, "knob.%s not stored: %s", key, esp_err_to_name(e));
    return e == ESP_OK;
}

bool knob_config_set_int(const char *key, int32_t v) { return set_done(key, espos_config_set_i32(NS, key, v)); }
bool knob_config_set_bool(const char *key, bool v) { return set_done(key, espos_config_set_bool(NS, key, v)); }
bool knob_config_set_str(const char *key, const char *v) { return set_done(key, espos_config_set_str(NS, key, v)); }

void knob_config_clear_wifi(void)
{
    for (int i = 0; i < 4; i++) {
        char k[8];
        snprintf(k, sizeof(k), "ssid%d", i);
        espos_config_set_str("wifi", k, "");
        snprintf(k, sizeof(k), "psk%d", i);
        espos_config_set_str("wifi", k, "");
    }
    ESP_LOGW(TAG, "Wi-Fi networks cleared");
}

static void (*s_watch_cb)(const char *key);

static void on_change(const char *ns, const char *key, void *arg)
{
    (void)arg;
    if (s_watch_cb && ns && key && !strcmp(ns, NS)) s_watch_cb(key);
}

void knob_config_watch(void (*cb)(const char *key))
{
    s_watch_cb = cb;
    if (espos_config_subscribe(on_change, NULL) != ESP_OK) ESP_LOGW(TAG, "no config subscription slot: changes need a restart");
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
