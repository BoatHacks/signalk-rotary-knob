/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_model.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espos_sk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_actions.h"
#include "knob_config.h"
#include "knob_hid.h"
#include "knob_logic.h"
#include "knob_net.h"
#include "knob_power.h"
#include "lv_port.h"
#include "usb_dev.h"

static const char *TAG = "knob_model";

/* ------------------------------------------------------------------------------------------------------------------
 * Configuration. Compiled in for now, the knob_config component (NVS and the espOS settings page) replaces it.
 * ---------------------------------------------------------------------------------------------------------------- */

/* Loaded from the espOS settings store (knob_config) at start: the autopilot id, the heading source, the default anchor radius
 * and the light channels (name, path, relay or dimmer). Changes on the settings page apply after a restart. */
static knob_cfg_t s_cfg;
#define s_lights s_cfg.lights
#define N_LIGHTS s_cfg.n_lights

/* Stale timeouts, ms. 0 = never stale by age (event driven values). The server publishes no meta.timeout for these paths. */
#define T_HEADING 5000
#define T_AP      8000   /* the autopilot is polled every 2 s */
#define T_ANCHOR  10000

#define T_GPS_LOST   30000  /* armed anchor and no position update for this long: GPS lost alarm */
#define T_BOOT_GRACE 45000  /* no GPS lost alarm in the first seconds after boot */

/* ------------------------------------------------------------------------------------------------------------------
 * State, written by the stream callback and the worker, read by the model task. One short critical section each.
 * ---------------------------------------------------------------------------------------------------------------- */

typedef struct {
    bool night_known, night;
    double heading_true, heading_mag;           /* rad */
    uint32_t heading_true_ms, heading_mag_ms;   /* knob time of arrival, 0 = never */
    bool gps_known, gps_fix;
    bool pos_valid;
    double lat, lon;
    uint32_t pos_ms;
    int8_t light[8];                            /* -1 unknown, 0 off, 1 on */
    uint32_t light_ms;
    bool anchor_seen, anchor_on;
    float anchor_curr_m, anchor_max_m, anchor_dist_bow_m, anchor_bearing_rad;
    uint32_t anchor_ms;
    char alarm_state[12];                       /* notifications.navigation.anchor state, "" none */
    bool ap_known, ap_engaged, ap_has_target;
    double ap_target_rad;
    uint32_t ap_ms;
    uint32_t last_rx_ms;
    /* Stale timeouts in ms. Defaults from the T_* constants, replaced by the key's meta.timeout (seconds) when the server
     * publishes one. 0 = never stale by age. */
    uint32_t to_heading_true, to_heading_mag, to_anchor;
} model_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static model_t s_m;

static volatile bool s_heading_frozen;
static volatile bool s_bypass_net;
static volatile uint8_t s_action_result;      /* what became of the last anchor action, marine_action_result_t */
static volatile uint16_t s_action_result_seq; /* 0 = none yet, changes with every result */
static volatile bool s_force_down;
static volatile int s_night_override;   /* 0 follow the server, 1 night, 2 day */
static volatile int s_alarm_inject;     /* 0 none, else marine_alarm_type_t */
static volatile bool s_ack_latch;

static volatile int s_phase;
static volatile uint32_t s_loop_ms, s_apply_done_ms;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------------------------------------------------------
 * Stream callback: runs on the espOS stream task. Copy and return, never block.
 * ---------------------------------------------------------------------------------------------------------------- */

/* The value helpers live in knob_logic (pure C, unit tested on a host). */
#define parse_num knob_parse_num
#define parse_str knob_parse_str
#define obj_num knob_obj_num
#define obj_str knob_obj_str

static void on_update(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    if (!u->path) return;
    if (u->meta_json) { /* a meta item: only the timeout is used */
        double t;
        if (obj_num(u->meta_json, "timeout", &t) && t >= 0) {
            uint32_t ms_to = (uint32_t)(t * 1000.0);
            portENTER_CRITICAL(&s_mux);
            if (!strcmp(u->path, "navigation.headingTrue")) s_m.to_heading_true = ms_to;
            else if (!strcmp(u->path, "navigation.headingMagnetic")) s_m.to_heading_mag = ms_to;
            else if (!strncmp(u->path, "navigation.anchor.", 18)) s_m.to_anchor = ms_to;
            portEXIT_CRITICAL(&s_mux);
        }
        return;
    }
    if (!u->value_json) return;
    const char *p = u->path, *v = u->value_json;
    uint32_t now = ms_now();
    double d;
    char s[16];

    portENTER_CRITICAL(&s_mux);
    s_m.last_rx_ms = now;
    portEXIT_CRITICAL(&s_mux);

    if (!strcmp(p, "environment.mode")) {
        if (parse_str(v, s, sizeof(s))) {
            portENTER_CRITICAL(&s_mux);
            s_m.night_known = true;
            s_m.night = !strcmp(s, "night");
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (!strcmp(p, "navigation.headingTrue")) {
        if (!s_heading_frozen && parse_num(v, &d)) {
            portENTER_CRITICAL(&s_mux);
            s_m.heading_true = d; s_m.heading_true_ms = now;
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (!strcmp(p, "navigation.headingMagnetic")) {
        if (!s_heading_frozen && parse_num(v, &d)) {
            portENTER_CRITICAL(&s_mux);
            s_m.heading_mag = d; s_m.heading_mag_ms = now;
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (!strcmp(p, "navigation.gnss.methodQuality")) {
        char q[24];
        if (parse_str(v, q, sizeof(q))) {
            portENTER_CRITICAL(&s_mux);
            s_m.gps_known = true;
            s_m.gps_fix = strstr(q, "no GPS") == NULL;
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (!strcmp(p, "navigation.position")) {
        double la, lo;
        if (obj_num(v, "latitude", &la) && obj_num(v, "longitude", &lo)) {
            portENTER_CRITICAL(&s_mux);
            s_m.pos_valid = true; s_m.lat = la; s_m.lon = lo; s_m.pos_ms = now;
            portEXIT_CRITICAL(&s_mux);
        }
    } else if (!strncmp(p, "navigation.anchor.", 18)) {
        const char *k = p + 18;
        portENTER_CRITICAL(&s_mux);
        s_m.anchor_seen = true;
        s_m.anchor_ms = now;
        if (!strcmp(k, "state") && parse_str(v, s, sizeof(s))) {
            s_m.anchor_on = !strcmp(s, "on");
        } else if (!strcmp(k, "currentRadius")) {
            s_m.anchor_curr_m = parse_num(v, &d) ? (float)d : 0;
        } else if (!strcmp(k, "maxRadius")) {
            s_m.anchor_max_m = parse_num(v, &d) ? (float)d : 0;
        } else if (!strcmp(k, "distanceFromBow")) {
            s_m.anchor_dist_bow_m = parse_num(v, &d) ? (float)d : 0;
        } else if (!strcmp(k, "bearingTrue")) {
            s_m.anchor_bearing_rad = parse_num(v, &d) ? (float)d : 0;
        }
        portEXIT_CRITICAL(&s_mux);
    } else if (!strcmp(p, "notifications.navigation.anchor")) {
        char st[12] = "";
        if (strcmp(v, "null") && obj_str(v, "state", st, sizeof(st))) {
            /* keep it */
        }
        portENTER_CRITICAL(&s_mux);
        strncpy(s_m.alarm_state, st, sizeof(s_m.alarm_state) - 1);
        s_m.alarm_state[sizeof(s_m.alarm_state) - 1] = '\0';
        portEXIT_CRITICAL(&s_mux);
    } else {
        for (int i = 0; i < N_LIGHTS; i++) {
            if (!strcmp(p, s_lights[i].path)) {
                int8_t val = -1;
                if (parse_num(v, &d)) val = d > 0.5 ? 1 : 0;
                else if (!strcmp(v, "true")) val = 1;
                else if (!strcmp(v, "false")) val = 0;
                portENTER_CRITICAL(&s_mux);
                s_m.light[i] = val;
                s_m.light_ms = now;
                portEXIT_CRITICAL(&s_mux);
                break;
            }
        }
    }
}

static void subscribe_all(void)
{
    static const struct { const char *pat; uint32_t period; } subs[] = {
        { "environment.mode", 0 },
        { "navigation.headingTrue", 500 },
        { "navigation.headingMagnetic", 500 },
        { "navigation.gnss.methodQuality", 2000 },
        { "navigation.position", 2000 },
        { "navigation.anchor.*", 1000 },
        { "notifications.navigation.anchor", 0 },
    };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        if (espos_sk_subscribe(subs[i].pat, subs[i].period, on_update, NULL) <= 0) ESP_LOGW(TAG, "subscribe %s failed", subs[i].pat);
    }
    for (int i = 0; i < N_LIGHTS; i++) {
        if (espos_sk_subscribe(s_lights[i].path, 0, on_update, NULL) <= 0) ESP_LOGW(TAG, "subscribe %s failed", s_lights[i].path);
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Snapshot
 * ---------------------------------------------------------------------------------------------------------------- */

#define RAD2DEG 57.29577951308232

#define stale_by knob_stale_by

static void build_snapshot(marine_ui_snapshot_t *s, uint32_t now)
{
    model_t m;
    portENTER_CRITICAL(&s_mux);
    m = s_m;
    portEXIT_CRITICAL(&s_mux);

    knob_net_status_t ns;
    knob_net_get(&ns);

    s->now_ms = now;
    s->fx_glass_enabled = true;
    s->fx_glow_enabled = true;
    s->fx_ring_pulse_enabled = true;
    s->alarm_ack_return_with_badge = true;

    /* System screen from the real network phase. */
    if (s_bypass_net) {
        s->system_screen = MARINE_SCREEN_OPERATIONAL;
    } else {
        switch (ns.phase) {
        case KNOB_NET_PORTAL:
            s->system_screen = MARINE_SCREEN_WIFI_PORTAL;
            strncpy(s->setup_ap_ssid, ns.portal_ssid, sizeof(s->setup_ap_ssid) - 1);
            break;
        case KNOB_NET_PAIRING:
            s->system_screen = MARINE_SCREEN_PAIRING;
            strncpy(s->device_name, ns.hostname, sizeof(s->device_name) - 1);
            break;
        case KNOB_NET_FAILED: s->system_screen = MARINE_SCREEN_CANNOT_CONNECT; break;
        case KNOB_NET_ONLINE: s->system_screen = MARINE_SCREEN_OPERATIONAL; break;
        default: s->system_screen = MARINE_SCREEN_BOOT; break;
        }
        strncpy(s->boot_phase, ns.text, sizeof(s->boot_phase) - 1);
        s->boot_progress_pct = ns.progress;
    }

    /* Night palette: follows environment.mode, default day until the first value. */
    bool night = m.night_known && m.night;
    if (s_night_override == 1) night = true;
    else if (s_night_override == 2) night = false;
    s->night_palette = night;
    knob_power_set_night(night);

    /* Link */
    bool online = ns.phase == KNOB_NET_ONLINE && !s_force_down;
    s->signalk_connected = s_bypass_net ? !s_force_down : online;
    s->last_data_ms = m.last_rx_ms;

    /* Heading: true by default, magnetic as the fallback. */
    bool true_ok = m.heading_true_ms != 0 && !stale_by(now, m.heading_true_ms, m.to_heading_true);
    bool mag_ok = m.heading_mag_ms != 0 && !stale_by(now, m.heading_mag_ms, m.to_heading_mag);
    bool use_true = s_cfg.heading_true ? (true_ok || !mag_ok) : (!mag_ok && true_ok ? true : false);
    double hd = use_true ? m.heading_true : m.heading_mag;
    uint32_t hms = use_true ? m.heading_true_ms : m.heading_mag_ms;
    hd *= RAD2DEG;
    if (hd < 0) hd += 360.0;
    if (hd >= 360.0) hd -= 360.0;
    s->heading_ref = use_true ? 'T' : 'M';
    s->heading_deg = (float)hd;
    s->heading_updated_ms = hms;
    s->heading_timeout_ms = use_true ? m.to_heading_true : m.to_heading_mag;

    /* Autopilot: from the REST poll. In standby with no target the dial starts from the current heading. */
    s->ap_engaged = m.ap_known && m.ap_engaged;
    s->ap_state_updated_ms = m.ap_ms;
    s->ap_state_timeout_ms = T_AP;
    double ct = m.ap_has_target ? m.ap_target_rad * RAD2DEG : hd;
    if (ct < 0) ct += 360.0;
    if (ct >= 360.0) ct -= 360.0;
    s->cts_deg = (float)ct;
    s->cts_updated_ms = m.ap_ms;
    s->cts_timeout_ms = T_AP;

    /* Telemetry this boat does not publish: updated 0 with a timeout shows --- . */
    s->rudder_updated_ms = 0; s->rudder_timeout_ms = T_HEADING;
    s->depth_updated_ms = 0; s->depth_timeout_ms = 10000;
    s->voltage_updated_ms = 0; s->voltage_timeout_ms = 30000;
    s->gps_3d_fix = m.gps_known && m.gps_fix;
    s->gps_updated_ms = now; s->gps_timeout_ms = 0;

    /* Anchor watch (hoekens-anchor-alarm). Not armed: nothing to go stale. */
    s->anchor_dropped = m.anchor_on;
    s->anchor_radius_m = (m.anchor_on && m.anchor_max_m > 0) ? (uint16_t)lroundf(m.anchor_max_m) : knob_actions_anchor_radius();
    s->anchor_dist_m = m.anchor_curr_m;
    double br = m.anchor_bearing_rad * RAD2DEG;
    if (br < 0) br += 360.0;
    s->anchor_bearing_deg = (float)br;
    s->anchor_updated_ms = m.anchor_on ? m.anchor_ms : now;
    s->anchor_timeout_ms = m.anchor_on ? m.to_anchor : 0;

    /* Alarm: the plugin's notification, or an injected one (bench). */
    marine_alarm_type_t alarm = MARINE_ALARM_NONE;
    if (!strcmp(m.alarm_state, "alarm") || !strcmp(m.alarm_state, "emergency")) alarm = MARINE_ALARM_ANCHOR_DRAG;
    if (s_alarm_inject) alarm = (marine_alarm_type_t)s_alarm_inject;
    /* GPS lost: the anchor watch is armed but the position has gone stale or the fix is gone. Not within the first 45 s after
     * boot, the subscriptions need a moment. A drag alarm takes precedence. */
    if (alarm == MARINE_ALARM_NONE &&
        knob_gps_lost(m.anchor_on, now, T_BOOT_GRACE, m.pos_valid, m.pos_ms, T_GPS_LOST, m.gps_known, m.gps_fix)) {
        alarm = MARINE_ALARM_GPS_LOST;
    }
    if (alarm == MARINE_ALARM_NONE) s_ack_latch = false;
    s->active_alarm = alarm;
    s->alarm_acknowledged = alarm != MARINE_ALARM_NONE && s_ack_latch;
    s->action_result = s_action_result;
    s->action_result_seq = s_action_result_seq;

    /* Lights */
    s->dimmer_group_count = N_LIGHTS;
    for (int i = 0; i < N_LIGHTS && i < MARINE_MAX_DIMMER_GROUPS; i++) {
        marine_dimmer_group_t *g = &s->dimmer_groups[i];
        memset(g, 0, sizeof(*g));
        strncpy(g->name, s_lights[i].name, sizeof(g->name) - 1);
        g->onoff_only = s_lights[i].onoff;
        g->on = m.light[i] == 1;
        g->level_pct = g->on ? 100 : 0;
    }
    s->dimmer_updated_ms = m.light_ms;
    s->dimmer_timeout_ms = 0;

    s->usb_host_enumerated = usb_dev_mounted();
    s->hid_profile = knob_hid_profile();
    if (!s->device_name[0]) strncpy(s->device_name, ns.hostname, sizeof(s->device_name) - 1);
}

static void model_task(void *arg)
{
    static marine_ui_snapshot_t snap; /* big, keep it off the stack */
    memset(&snap, 0, sizeof(snap));
    for (;;) {
        s_loop_ms = ms_now();
        s_phase = 1;
        build_snapshot(&snap, ms_now());
        knob_power_apply_snapshot(&snap, marine_ui_get_active_mode());
        s_phase = 2;
        lv_port_lock();
        s_phase = 3;
        marine_ui_apply_snapshot(&snap);
        lv_port_unlock();
        s_apply_done_ms = ms_now();
        s_phase = 4;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Adapters, on the worker task: they may block on HTTP.
 * ---------------------------------------------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------------------------- */

/* What the adapters call back into the model for. */
static void hook_get_view(knob_actions_view_t *v)
{
    portENTER_CRITICAL(&s_mux);
    memcpy(v->light, s_m.light, sizeof(v->light));
    v->anchor_on = s_m.anchor_on;
    v->anchor_max_m = s_m.anchor_max_m;
    v->pos_valid = s_m.pos_valid;
    v->lat = s_m.lat;
    v->lon = s_m.lon;
    v->pos_ms = s_m.pos_ms;
    portEXIT_CRITICAL(&s_mux);
}

static void hook_set_autopilot(bool engaged, bool has_target, double target_rad)
{
    portENTER_CRITICAL(&s_mux);
    s_m.ap_known = true;
    s_m.ap_engaged = engaged;
    s_m.ap_has_target = has_target;
    s_m.ap_target_rad = target_rad;
    s_m.ap_ms = ms_now();
    portEXIT_CRITICAL(&s_mux);
}

static void hook_ack_latch(void) { s_ack_latch = true; }
static bool hook_alarm_injected(void) { return s_alarm_inject != 0; }

static void hook_action_result(uint8_t code)
{
    s_action_result = code;
    uint16_t seq = (uint16_t)(s_action_result_seq + 1);
    s_action_result_seq = seq ? seq : 1; /* 0 stays "none yet" */
}

static const knob_actions_hooks_t s_hooks = {
    .get_view = hook_get_view,
    .set_autopilot = hook_set_autopilot,
    .ack_latch = hook_ack_latch,
    .alarm_injected = hook_alarm_injected,
    .action_result = hook_action_result,
};

void knob_model_action_cb(const marine_ui_action_t *act, void *ctx)
{
    (void)ctx;
    /* Backlight and LED actions are cheap and must not wait behind a slow HTTP call: do them right here. The LVGL lock is
     * held by the caller, so nothing here may take it or block. */
    switch (act->action) {
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)act->value); return;
    case MARINE_ACT_ALARM_LED_PULSE: knob_power_led_alarm(act->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF); return;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE: knob_power_led_alarm(act->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF); return;
    case MARINE_ACT_ALARM_ACK: s_ack_latch = true; break; /* and the server call below */
    default: break;
    }
    knob_actions_submit(act);
}

void knob_model_command(char c)
{
    switch (c) {
    case 'a': s_alarm_inject = MARINE_ALARM_ANCHOR_DRAG; s_ack_latch = false; break;
    case 'g': s_alarm_inject = MARINE_ALARM_GPS_LOST; s_ack_latch = false; break;
    case 'c': s_alarm_inject = 0; break;
    case 'x': s_force_down = !s_force_down; break;
    case 's': s_heading_frozen = !s_heading_frozen; break;
    case 'n': s_night_override = (s_night_override + 1) % 3; break;
    case 'd': s_bypass_net = !s_bypass_net; break;
    case 'l':
        knob_actions_set_live(!knob_actions_is_live());
        return;
    default:
        ESP_LOGI(TAG, "commands: a anchor alarm, g gps alarm, c clear, x stream down, s freeze heading, n night override, "
                      "d bypass network gating, l live control (now %s)", knob_actions_is_live() ? "ON" : "off");
        return;
    }
    ESP_LOGI(TAG, "command %c", c);
}

void knob_model_get_diag(int *phase, uint32_t *loop_age_ms, uint32_t *apply_age_ms)
{
    uint32_t n = ms_now();
    *phase = s_phase;
    *loop_age_ms = n - s_loop_ms;
    *apply_age_ms = n - s_apply_done_ms;
}

void knob_model_start(void)
{
    memset(&s_m, 0, sizeof(s_m));
    for (int i = 0; i < 8; i++) s_m.light[i] = -1;
    s_m.to_heading_true = s_m.to_heading_mag = T_HEADING;
    s_m.to_anchor = T_ANCHOR;
    strcpy(s_cfg.autopilot_id, "raymarineST"); /* defaults until knob_model_subscribe() loads the settings */
    s_cfg.heading_true = true;
    s_cfg.anchor_radius_m = 35;
    xTaskCreate(model_task, "model", 6144, NULL, 2, NULL);
    knob_actions_start(&s_cfg, &s_hooks);
    ESP_LOGI(TAG, "model started, live control off");
}

void knob_model_subscribe(void)
{
    /* The settings store is up once espos_start() has returned. The tasks already run on the defaults. */
    static knob_cfg_t cfg;
    knob_config_load(&cfg);
    portENTER_CRITICAL(&s_mux);
    s_cfg = cfg;
    portEXIT_CRITICAL(&s_mux);
    knob_actions_set_anchor_radius(s_cfg.anchor_radius_m);
    lv_port_lock();
    marine_ui_set_power(cfg.dim_after_ms, cfg.dim_level_pct, cfg.off_after_ms);
    lv_port_unlock();
    subscribe_all();
    ESP_LOGI(TAG, "subscribed, %d light channel(s), autopilot '%s'", N_LIGHTS, s_cfg.autopilot_id);
}
