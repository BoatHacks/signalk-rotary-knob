/* SPDX-License-Identifier: Apache-2.0
 * The knob's own settings, stored in the espOS config store (NVS) under the "knob" namespace and edited on the espOS settings
 * page or through its REST config API. Descriptor: config/knob.json. The light channels apply after a restart, the other keys
 * at once (knob_config_watch). */
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
    bool allow_control;      /* autopilot and anchor commands may leave the knob (SPEC section 2), off by default */
    char autopilot_id[32];
    bool heading_true;       /* true: navigation.headingTrue, false: navigation.headingMagnetic */
    uint16_t anchor_radius_m;
    uint32_t dim_after_ms;   /* idle time before the backlight dims */
    uint8_t dim_level_pct;
    uint32_t off_after_ms;   /* idle time before the backlight goes off, 0 = never */
    uint8_t brightness_pct;  /* backlight while in use */
    uint8_t night_mode;      /* 0 follow environment.mode, 1 day, 2 night */
    bool leds_on;            /* false: every LED dark, the alarm included */
    uint8_t hid_profile;     /* usb_hid_profile_t: 0 chartplotter, 1 media, 2 nav */
    int n_lights;            /* channels with a non-empty path, compacted to the front of lights[] */
    knob_cfg_light_t lights[KNOB_CFG_MAX_LIGHTS];
} knob_cfg_t;

/* Reads the settings. Falls back to the descriptor defaults, so it always fills in something usable. Reads the settings store:
 * call it from a task whose stack is in internal RAM. */
void knob_config_load(knob_cfg_t *out);

/* Writes one key of the knob namespace (the settings screen). The change reaches the watcher like any other. Writes flash:
 * call it from a task whose stack is in internal RAM (the LVGL task). */
bool knob_config_set_int(const char *key, int32_t v);
bool knob_config_set_bool(const char *key, bool v);
bool knob_config_set_str(const char *key, const char *v);

/* Reset setup: forgets the Wi-Fi networks (espOS's wifi.ssid0..3 and their passwords), so the setup access point opens at the
 * next start. Writes flash: internal RAM stack only. */
void knob_config_clear_wifi(void);

/* Calls cb(key) after every committed change of a key in the "knob" namespace, from the settings page, the config API or an
 * import. It runs on the writer's task (espOS's httpd task, stack in internal RAM), so it may call knob_config_load(). One
 * watcher. Call after espos_start(). */
void knob_config_watch(void (*cb)(const char *key));

/* The last mode, kept in its own NVS namespace ("knob_state"), not on the settings page: the last non-HID mode (-1 when none
 * is stored) and whether USB-HID was on screen after it. Call after espos_start() (NVS is up then). */
int knob_config_last_mode(void);
bool knob_config_last_hid(void);
/* Stores a mode change when it differs from what is stored. hid true: USB-HID was chosen, mode is ignored and the last non-HID
 * mode stays. Writes flash, which stops the caches: call it only from a task whose stack is in internal RAM (the LVGL task),
 * never from one with a PSRAM stack (input, model, worker). */
void knob_config_save_last_mode(int mode, bool hid);
