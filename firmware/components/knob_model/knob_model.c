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
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "knob_actions.h"
#include "knob_config.h"
#include "knob_hid.h"
#include "espos_wifi.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "knob_logic.h"
#include "knob_demo.h"
#include "knob_net.h"
#include "knob_power.h"
#include "lv_port.h"
#include "lvgl.h"
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
#define T_BOOT_HID 10000    /* boot window in which a USB host brings back the USB-HID mode */
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
static bool s_token_verified;           /* the token was verified since boot: the splash is done (model task only) */
static volatile uint16_t s_control_on_seq;  /* counts allow control going on from the web page or the API */
static volatile int s_last_alarm;            /* marine_alarm_type_t of the latest snapshot, for the status API */
static volatile bool s_boot_hid;        /* USB-HID was the last mode and the boot window is open */
static marine_ui_mode_t s_boot_mode;    /* the mode restored at boot */
static volatile int s_night_override;   /* 0 follow the server, 1 night, 2 day: the stored night_mode, serial n changes it
                                           for the session */
static volatile bool s_write_from_knob;  /* a setting is being stored from the settings screen (no CONTROL ON notice) */
static volatile uint32_t s_ap_until_ms;  /* the setup access point opened from the settings closes then, 0 = not open */
#define T_SETUP_AP 600000                /* 10 min */
static void setup_ap(bool open);

/* Demo mode (#4): chosen at boot from knob.demo. The simulation writes the model state that SignalK would, so the snapshot is
 * built the same way, and the UI's actions are applied to it instead of being sent. */
static bool s_demo;
static knob_demo_t s_dm;
static void hook_action_result(uint8_t code);
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

/* The settings screen's values and read-only pages. Only while it is open: espOS status copies are cheap but not free. */
static void settings_snapshot(marine_ui_snapshot_t *s, const knob_net_status_t *ns)
{
    knob_cfg_t c;
    portENTER_CRITICAL(&s_mux);
    c = s_cfg;
    portEXIT_CRITICAL(&s_mux);
    s->setting[KNOB_SET_BRIGHTNESS] = c.brightness_pct;
    s->setting[KNOB_SET_DIM_AFTER] = (int16_t)(c.dim_after_ms / 1000);
    s->setting[KNOB_SET_DIM_LEVEL] = c.dim_level_pct;
    s->setting[KNOB_SET_OFF_AFTER] = (int16_t)(c.off_after_ms / 60000);
    s->setting[KNOB_SET_NIGHT] = c.night_mode;
    s->setting[KNOB_SET_LEDS] = c.leds_on ? 1 : 0;
    s->setting[KNOB_SET_HEADING] = c.heading_true ? 0 : 1;
    s->setting[KNOB_SET_HID_PROFILE] = c.hid_profile;
    s->setting[KNOB_SET_ALLOW_CONTROL] = c.allow_control ? 1 : 0;
    s->setting[KNOB_SET_DEMO] = c.demo ? 1 : 0;

    espos_wifi_status_t w;
    if (espos_wifi_get_status(&w) == ESP_OK) {
        s->setup_ap_open = w.sm.portal_active;
        if (w.sm.state == ESPOS_WIFI_ST_CONNECTED) {
            strncpy(s->info_wifi, w.sm.link.ssid, sizeof(s->info_wifi) - 1);
            strncpy(s->info_ip, w.sm.ip.ip, sizeof(s->info_ip) - 1);
            s->info_rssi = w.rssi;
        }
    }
    uint32_t left = s_ap_until_ms ? s_ap_until_ms - ms_now() : 0;
    s->setup_ap_left_s = (s_ap_until_ms && (int32_t)left > 0) ? (uint16_t)(left / 1000) : 0;
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) strncpy(s->info_firmware, app->version, sizeof(s->info_firmware) - 1);
    s->info_free_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    s->info_uptime_s = ms_now() / 1000;
    espos_sk_server_t srv;
    if (espos_sk_get_server(&srv) == ESP_OK && srv.host[0]) {
        char port[8];
        snprintf(port, sizeof(port), ":%u", (unsigned)srv.port);
        size_t hl = strnlen(srv.host, sizeof(s->info_server) - sizeof(port)); /* a long host name is cut, the port stays */
        memcpy(s->info_server, srv.host, hl);
        memcpy(s->info_server + hl, port, strlen(port) + 1);
    }
    knob_model_status_t st;
    knob_model_get_status(&st);
    strncpy(s->info_link, st.link, sizeof(s->info_link) - 1);
    const char *t = ns->token_state;
    strncpy(s->info_token, !strcmp(t, "approved") || !strcmp(t, "open") ? "approved" : !strcmp(t, "pending") ? "waiting"
                           : t[0] ? t : "none", sizeof(s->info_token) - 1);
    s->info_allow_control = knob_actions_is_live();
}

#define DEG2RAD 0.017453292519943295

static void demo_feed(uint32_t now)
{
    portENTER_CRITICAL(&s_mux);
    knob_demo_step(&s_dm, now);
    s_m.heading_true = s_dm.heading_deg * DEG2RAD;
    s_m.heading_mag = s_m.heading_true;
    s_m.heading_true_ms = s_m.heading_mag_ms = now;
    s_m.ap_known = true;
    s_m.ap_engaged = s_dm.engaged;
    s_m.ap_has_target = true;
    s_m.ap_target_rad = s_dm.target_deg * DEG2RAD;
    s_m.ap_ms = now;
    s_m.anchor_seen = true;
    s_m.anchor_on = s_dm.anchor_down;
    s_m.anchor_max_m = s_dm.radius_m;
    s_m.anchor_curr_m = s_dm.anchor_dist_m;
    s_m.anchor_bearing_rad = s_dm.anchor_bearing_deg * DEG2RAD;
    s_m.anchor_ms = now;
    snprintf(s_m.alarm_state, sizeof(s_m.alarm_state), "%s", knob_demo_dragging(&s_dm) ? "alarm" : "");
    s_m.gps_known = s_m.gps_fix = true;
    s_m.pos_valid = true;
    s_m.pos_ms = now;
    for (int i = 0; i < N_LIGHTS && i < KNOB_DEMO_LIGHTS; i++) s_m.light[i] = s_dm.light[i] ? 1 : 0;
    s_m.light_ms = now;
    s_m.last_rx_ms = now;
    portEXIT_CRITICAL(&s_mux);
}

/* The UI's actions in demo mode: applied to the simulation, never sent. Anchor actions report OK so the pending screen ends. */
static void demo_action(const marine_ui_action_t *a)
{
    portENTER_CRITICAL(&s_mux);
    bool lit = false, report = false;
    switch (a->action) {
    case MARINE_ACT_AP_ENGAGE: knob_demo_engage(&s_dm, true); break;
    case MARINE_ACT_AP_STANDBY: knob_demo_engage(&s_dm, false); break;
    case MARINE_ACT_AP_SET_CTS: knob_demo_set_target(&s_dm, (float)a->value); break;
    case MARINE_ACT_ANCHOR_SET_RADIUS: knob_demo_set_radius(&s_dm, (uint16_t)a->value); report = s_dm.anchor_down; break;
    case MARINE_ACT_ANCHOR_DROP:
    case MARINE_ACT_ANCHOR_MOVE_DROP: knob_demo_drop(&s_dm, 0); report = true; break;
    case MARINE_ACT_ANCHOR_RAISE: knob_demo_raise(&s_dm); report = true; break;
    case MARINE_ACT_DIMMER_TOGGLE_POWER:
        if (a->value >= 0 && a->value < KNOB_DEMO_LIGHTS) knob_demo_set_light(&s_dm, (int)a->value, !s_dm.light[a->value]);
        lit = true;
        break;
    case MARINE_ACT_DIMMER_SET_LEVEL: knob_demo_set_light(&s_dm, (int)a->meta, a->value > 0); lit = true; break;
    default: break;
    }
    portEXIT_CRITICAL(&s_mux);
    if (a->action == MARINE_ACT_ANCHOR_SET_RADIUS) knob_actions_set_anchor_radius((uint16_t)a->value);
    if (report) hook_action_result(MARINE_RESULT_OK);
    (void)lit;
}

static void build_snapshot(marine_ui_snapshot_t *s, uint32_t now)
{
    model_t m;
    portENTER_CRITICAL(&s_mux);
    m = s_m;
    portEXIT_CRITICAL(&s_mux);

    knob_net_status_t ns;
    knob_net_get(&ns);

    s->now_ms = now;
    s->control_on_seq = s_control_on_seq;
    if (marine_ui_get_active_mode() == MARINE_MODE_SETTINGS) settings_snapshot(s, &ns);
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
        case KNOB_NET_ONLINE:
            s->system_screen = MARINE_SCREEN_OPERATIONAL;
            s_token_verified = true;
            break;
        default:
            /* The splash shows the phases until the token is verified, then the last mode opens (CONNECTING until the first
             * data) and stays when the link drops later (LINK LOST). */
            s->system_screen = knob_boot_show_mode(ns.phase == KNOB_NET_LINK, &s_token_verified) ? MARINE_SCREEN_OPERATIONAL
                                                                                                  : MARINE_SCREEN_BOOT;
            break;
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
    s_last_alarm = (int)alarm;
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

    s->usb_host_enumerated = usb_dev_host_present(); /* a host on the cable: the USB-HID mode is offered on the ring */
    s->hid_profile = knob_hid_profile();
    if (!s->device_name[0]) strncpy(s->device_name, ns.hostname, sizeof(s->device_name) - 1);

    if (s_demo) {
        /* No SignalK: the mode screens show the simulation, also while the setup access point runs. */
        s->system_screen = MARINE_SCREEN_OPERATIONAL;
        s->signalk_connected = !s_force_down;
        s->demo = true;
        portENTER_CRITICAL(&s_mux);
        s->rudder_deg = s_dm.rudder_deg;
        s->depth_m = s_dm.depth_m;
        s->voltage_v = s_dm.voltage_v;
        portEXIT_CRITICAL(&s_mux);
        s->rudder_updated_ms = s->depth_updated_ms = s->voltage_updated_ms = now;
    }
}

static void model_task(void *arg)
{
    static marine_ui_snapshot_t snap; /* big, keep it off the stack */
    memset(&snap, 0, sizeof(snap));
    for (;;) {
        s_loop_ms = ms_now();
        s_phase = 1;
        if (s_demo) demo_feed(ms_now());
        build_snapshot(&snap, ms_now());
        marine_ui_mode_t mode = marine_ui_get_active_mode();
        if (s_boot_hid) {
            /* USB-HID was the last mode: switch to it once a USB host shows up within the boot window (ui-design 3.4). */
            uint32_t now = ms_now();
            if (knob_boot_hid(true, snap.usb_host_enumerated, now, T_BOOT_HID, mode != s_boot_mode)) {
                lv_port_lock();
                marine_ui_set_mode(MARINE_MODE_USB_HID);
                lv_port_unlock();
                mode = MARINE_MODE_USB_HID;
                s_boot_hid = false;
                ESP_LOGI(TAG, "boot mode USB-HID, host present");
            } else if (now >= T_BOOT_HID || mode != s_boot_mode) {
                s_boot_hid = false;
            }
        }
        if (s_ap_until_ms && (int32_t)(ms_now() - s_ap_until_ms) >= 0) setup_ap(false);
        knob_power_apply_snapshot(&snap, mode);
        usb_dev_set_hid(mode == MARINE_MODE_USB_HID); /* a HID device only while the USB-HID mode is on screen */
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

static void hook_autopilot_id(char *out, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(out, n, "%s", s_cfg.autopilot_id);
    portEXIT_CRITICAL(&s_mux);
}

static const knob_actions_hooks_t s_hooks = {
    .get_view = hook_get_view,
    .set_autopilot = hook_set_autopilot,
    .ack_latch = hook_ack_latch,
    .alarm_injected = hook_alarm_injected,
    .action_result = hook_action_result,
    .autopilot_id = hook_autopilot_id,
};

static volatile int s_mode_to_save;

static void save_mode_async(void *arg)
{
    (void)arg;
    if (s_mode_to_save == MARINE_MODE_SETTINGS) return; /* a boot never opens the settings */
    knob_config_save_last_mode(s_mode_to_save, s_mode_to_save == MARINE_MODE_USB_HID);
}

/* A setting from the settings screen, stored on the LVGL task (internal RAM stack, may write flash). arg packs the item in the
 * high half and the value in the low half. */
static void setting_write_async(void *arg)
{
    uint32_t a = (uint32_t)(uintptr_t)arg;
    knob_setting_t item = (knob_setting_t)(a >> 16);
    int v = (int)(int16_t)(a & 0xFFFF);
    static const char *const night[] = { "server", "day", "night" };
    static const char *const hid[] = { "chartplotter", "media", "nav" };
    s_write_from_knob = true;
    switch (item) {
    case KNOB_SET_BRIGHTNESS: knob_config_set_int("brightness_pct", v); break;
    case KNOB_SET_DIM_AFTER: knob_config_set_int("dim_after_s", v); break;
    case KNOB_SET_DIM_LEVEL: knob_config_set_int("dim_level_pct", v); break;
    case KNOB_SET_OFF_AFTER: knob_config_set_int("off_after_min", v); break;
    case KNOB_SET_NIGHT: if (v >= 0 && v < 3) knob_config_set_str("night_mode", night[v]); break;
    case KNOB_SET_LEDS: knob_config_set_bool("leds_on", v != 0); break;
    case KNOB_SET_HEADING: knob_config_set_str("heading_source", v ? "magnetic" : "true"); break;
    case KNOB_SET_HID_PROFILE: if (v >= 0 && v < 3) knob_config_set_str("hid_profile", hid[v]); break;
    case KNOB_SET_ALLOW_CONTROL: knob_config_set_bool("allow_control", v != 0); break;
    case KNOB_SET_DEMO: knob_config_set_bool("demo", v != 0); break;
    default: break;
    }
    s_write_from_knob = false;
}

static void queue_setting(knob_setting_t item, int value)
{
    lv_async_call(setting_write_async, (void *)(uintptr_t)(((uint32_t)item << 16) | ((uint32_t)value & 0xFFFF)));
}

/* Reset setup: forget the Wi-Fi networks and the SignalK token, then restart into the setup access point. LVGL task. */
static void reset_setup_async(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "reset setup: Wi-Fi and pairing cleared, restarting");
    knob_config_clear_wifi();
    espos_sk_forget_token();
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the log line out */
    esp_restart();
}

static void setup_ap(bool open)
{
    if (open) {
        if (espos_wifi_portal_open(true) == ESP_OK) s_ap_until_ms = ms_now() + T_SETUP_AP;
        ESP_LOGI(TAG, "setup access point opened from the knob for 10 min");
    } else if (s_ap_until_ms) {
        s_ap_until_ms = 0;
        espos_wifi_portal_auto();
        ESP_LOGI(TAG, "setup access point back to automatic");
    }
}

void knob_model_action_cb(const marine_ui_action_t *act, void *ctx)
{
    (void)ctx;
    /* Backlight and LED actions are cheap and must not wait behind a slow HTTP call: do them right here. The LVGL lock is
     * held by the caller, so nothing here may take it or block. */
    switch (act->action) {
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)act->value); return;
    case MARINE_ACT_ALARM_LED_PULSE: knob_power_led_alarm(act->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF); return;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE: knob_power_led_alarm(act->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF); return;
    case MARINE_ACT_MODE_CHANGED:
        /* The input task that calls this has its stack in PSRAM, where a flash write must not run. The LVGL task's stack is
         * in internal RAM, so the write is handed to it. */
        s_mode_to_save = (int)act->value;
        lv_async_call(save_mode_async, NULL);
        return;
    case MARINE_ACT_SETTING_SET: queue_setting((knob_setting_t)act->value, (int)act->meta); return;
    case MARINE_ACT_SETUP_AP: setup_ap(act->value != 0); return;
    case MARINE_ACT_SYS_FACTORY_RESET: lv_async_call(reset_setup_async, NULL); return;
    case MARINE_ACT_HID_NEXT_PROFILE: /* stored, so it survives a restart. The change applies through the watcher. */
        queue_setting(KNOB_SET_HID_PROFILE, (knob_hid_profile() + 1) % HID_PROFILE_COUNT);
        return;
    case MARINE_ACT_ALARM_ACK: s_ack_latch = true; break; /* and the server call below */
    default: break;
    }
    if (s_demo && act->action != MARINE_ACT_HID_TURN && act->action != MARINE_ACT_HID_PRESS) {
        demo_action(act);
        return;
    }
    knob_actions_submit(act);
}

void knob_model_set_mode(int mode)
{
    lv_port_lock();
    marine_ui_set_mode((marine_ui_mode_t)mode);
    /* Stored like a ring change. The caller (httpd) has an internal RAM stack, but the write goes the same way as the ring's. */
    s_mode_to_save = mode;
    lv_async_call(save_mode_async, NULL);
    lv_port_unlock();
}

void knob_model_get_status(knob_model_status_t *out)
{
    knob_net_status_t ns;
    knob_net_get(&ns);
    switch (ns.phase) {
    case KNOB_NET_ONLINE: out->link = s_force_down ? "down" : "online"; break;
    case KNOB_NET_PORTAL: out->link = "setup"; break;
    case KNOB_NET_PAIRING: out->link = "pairing"; break;
    case KNOB_NET_FAILED: out->link = "failed"; break;
    default: out->link = "connecting"; break;
    }
    out->alarm = s_last_alarm;
    out->demo = s_demo;
}

void knob_model_restore_mode(void)
{
    int stored = knob_config_last_mode();
    s_boot_mode = (marine_ui_mode_t)knob_boot_mode(stored, MARINE_MODE_SETTINGS, MARINE_MODE_USB_HID);
    ESP_LOGI(TAG, "boot mode %d (stored %d, USB-HID last %d)", (int)s_boot_mode, stored, (int)knob_config_last_hid());
    lv_port_lock();
    marine_ui_set_mode(s_boot_mode);
    lv_port_unlock();
    s_boot_hid = knob_config_last_hid(); /* the model task switches to USB-HID if a host shows up in the boot window */
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
        knob_actions_toggle_override();
        return;
    default:
        ESP_LOGI(TAG, "commands: a anchor alarm, g gps alarm, c clear, x stream down, s freeze heading, n night override, "
                      "d bypass network gating, l allow control override for this session (control now %s)",
                 knob_actions_is_live() ? "ON" : "off");
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
    xTaskCreateWithCaps(model_task, "model", 6144, NULL, 2, NULL, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
    knob_actions_start(&s_cfg, &s_hooks);
    ESP_LOGI(TAG, "model started, allow control off until the settings are loaded");
}

/* A key of the knob namespace changed (settings page, config API, import). Runs on espOS's httpd task, whose stack is in
 * internal RAM, so it may read the settings store. The light channels stay as loaded at boot: new paths would need new
 * subscriptions, and this server closes the stream on an unsubscribe (ARCHITECTURE.md section 9). */
static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static int night_override_of(uint8_t night_mode) { return night_mode == 1 ? 2 : night_mode == 2 ? 1 : 0; }

/* Applies the settings that act outside the model (backlight, LEDs, HID, night). Not under any model lock: it may take the
 * LVGL lock. */
static void apply_device_settings(const knob_cfg_t *cfg)
{
    knob_power_set_brightness(cfg->brightness_pct);
    knob_power_set_leds(cfg->leds_on);
    knob_hid_set_profile((usb_hid_profile_t)cfg->hid_profile);
    s_night_override = night_override_of(cfg->night_mode);
    lv_port_lock();
    marine_ui_set_power(cfg->dim_after_ms, cfg->dim_level_pct, cfg->off_after_ms);
    lv_port_unlock();
}

/* A key of the knob namespace changed (settings page, config API, import, or the knob's settings screen). Runs on the
 * writer's task: espOS's httpd task or, for the settings screen, the LVGL task. Both have internal RAM stacks, so it may read
 * the settings store. The light channels stay as loaded at boot: new paths would need new subscriptions, and this server
 * closes the stream on an unsubscribe (ARCHITECTURE.md section 9). The mutex only covers the load and the copy, never the
 * LVGL lock, so a write from the LVGL task cannot wait on an httpd change that waits for the LVGL task. */
static void on_config_key(const char *key)
{
    static SemaphoreHandle_t lock;
    static knob_cfg_t cfg;
    if (!lock) lock = xSemaphoreCreateMutex();
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    knob_config_load(&cfg);
    knob_cfg_t now = cfg; /* the copy applied below, after the mutex is released */
    portENTER_CRITICAL(&s_mux);
    bool was_allowed = s_cfg.allow_control;
    s_cfg.allow_control = cfg.allow_control;
    memcpy(s_cfg.autopilot_id, cfg.autopilot_id, sizeof(s_cfg.autopilot_id));
    s_cfg.heading_true = cfg.heading_true;
    s_cfg.anchor_radius_m = cfg.anchor_radius_m;
    s_cfg.dim_after_ms = cfg.dim_after_ms;
    s_cfg.dim_level_pct = cfg.dim_level_pct;
    s_cfg.off_after_ms = cfg.off_after_ms;
    s_cfg.brightness_pct = cfg.brightness_pct;
    s_cfg.night_mode = cfg.night_mode;
    s_cfg.leds_on = cfg.leds_on;
    s_cfg.hid_profile = cfg.hid_profile;
    s_cfg.demo = cfg.demo;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(lock);
    bool from_knob = s_write_from_knob;
    ESP_LOGI(TAG, "setting knob.%s changed (%s)", key, from_knob ? "knob" : "web page or API");

    if (!strcmp(key, "allow_control")) {
        knob_actions_set_allow_control(now.allow_control, from_knob ? "knob" : "web page or API");
        if (now.allow_control && !was_allowed && !from_knob) {
            uint16_t n = (uint16_t)(s_control_on_seq + 1);
            s_control_on_seq = n ? n : 1; /* 0 means never */
        }
    } else if (!strcmp(key, "anchor_radius")) {
        knob_actions_set_anchor_radius(now.anchor_radius_m);
    } else if (!strcmp(key, "demo")) {
        if (now.demo != s_demo) {
            ESP_LOGW(TAG, "demo mode %s: restarting", now.demo ? "on" : "off");
            static esp_timer_handle_t t;
            const esp_timer_create_args_t a = { .callback = restart_cb, .name = "demo_rst" };
            if (!t && esp_timer_create(&a, &t) != ESP_OK) t = NULL;
            if (t) esp_timer_start_once(t, 800 * 1000); /* let the reply and the log line out first */
        }
    } else if (!strncmp(key, "light", 5)) {
        ESP_LOGW(TAG, "light channel settings apply after a restart");
    } else {
        apply_device_settings(&now);
    }
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
    knob_actions_set_allow_control(cfg.allow_control, "boot");
    apply_device_settings(&cfg);
    knob_config_watch(on_config_key);
    if (cfg.demo) {
        /* Demo mode: stop the SignalK client before Wi-Fi is even up, so not a byte goes to a server. Wi-Fi and the settings
         * page stay, to switch demo off from a phone. */
        espos_sk_stop();
        knob_actions_set_demo(true);
        knob_power_set_demo(true);
        portENTER_CRITICAL(&s_mux);
        knob_demo_init(&s_dm, ms_now(), cfg.anchor_radius_m);
        portEXIT_CRITICAL(&s_mux);
        s_demo = true;
        ESP_LOGW(TAG, "DEMO MODE: simulated data, the SignalK client is stopped, nothing is sent");
        return;
    }
    subscribe_all();
    ESP_LOGI(TAG, "subscribed, %d light channel(s), autopilot '%s'", N_LIGHTS, s_cfg.autopilot_id);
}
