/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_model.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos_sk.h"
#include "espos_sk_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_config.h"
#include "knob_net.h"
#include "knob_power.h"
#include "lv_port.h"
#include "usb_dev.h"

static const char *TAG = "knob_model";

/* ------------------------------------------------------------------------------------------------------------------
 * Configuration. Compiled in for now, the knob_config component (NVS and the espOS settings page) replaces it.
 * ---------------------------------------------------------------------------------------------------------------- */

#define ANCHOR_PLUGIN "/plugins/hoekens-anchor-alarm"

/* Loaded from the espOS settings store (knob_config) at start: the autopilot id, the heading source, the default anchor radius
 * and the light channels (name, path, relay or dimmer). Changes on the settings page apply after a restart. */
static knob_cfg_t s_cfg;
#define s_lights s_cfg.lights
#define N_LIGHTS s_cfg.n_lights
#define AP_BASE "/signalk/v2/api/vessels/self/autopilots/"

/* Stale timeouts, ms. 0 = never stale by age (event driven values). The server publishes no meta.timeout for these paths. */
#define T_HEADING 5000
#define T_AP      8000   /* the autopilot is polled every 2 s */
#define T_ANCHOR  10000

#define AP_POLL_MS 2000

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
} model_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static model_t s_m;

static QueueHandle_t s_q;
static volatile bool s_live;            /* LIVE CONTROL: autopilot and anchor commands are sent only while set */
static volatile bool s_heading_frozen;
static volatile bool s_bypass_net;
static volatile bool s_force_down;
static volatile int s_night_override;   /* 0 follow the server, 1 night, 2 day */
static volatile int s_alarm_inject;     /* 0 none, else marine_alarm_type_t */
static volatile bool s_ack_latch;
static volatile uint16_t s_radius_pref = 35;

static volatile int s_phase;
static volatile uint32_t s_loop_ms, s_apply_done_ms;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ------------------------------------------------------------------------------------------------------------------
 * Stream callback: runs on the espOS stream task. Copy and return, never block.
 * ---------------------------------------------------------------------------------------------------------------- */

static bool parse_num(const char *j, double *out)
{
    if (!j) return false;
    while (*j == ' ') j++;
    if (!(*j == '-' || *j == '+' || *j == '.' || (*j >= '0' && *j <= '9'))) return false;
    char *end = NULL;
    double v = strtod(j, &end);
    if (end == j) return false;
    *out = v;
    return true;
}

/* "text" -> text. Returns false when the value is not a JSON string. */
static bool parse_str(const char *j, char *out, size_t n)
{
    if (!j || j[0] != '"') return false;
    size_t i = 0;
    for (const char *p = j + 1; *p && *p != '"' && i + 1 < n; p++) out[i++] = *p;
    out[i] = '\0';
    return true;
}

/* A number after "key": inside a small JSON object text. */
static bool obj_num(const char *j, const char *key, double *out)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return parse_num(p, out);
}

static bool obj_str(const char *j, const char *key, char *out, size_t n)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return parse_str(p, out, n);
}

static void on_update(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    if (!u->path || !u->value_json) return; /* meta items are not used */
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

static bool stale_by(uint32_t now, uint32_t ms, uint32_t timeout) { return timeout != 0 && (ms == 0 || now - ms > timeout); }

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
    bool true_ok = m.heading_true_ms != 0 && !stale_by(now, m.heading_true_ms, T_HEADING);
    bool mag_ok = m.heading_mag_ms != 0 && !stale_by(now, m.heading_mag_ms, T_HEADING);
    bool use_true = s_cfg.heading_true ? (true_ok || !mag_ok) : (!mag_ok && true_ok ? true : false);
    double hd = use_true ? m.heading_true : m.heading_mag;
    uint32_t hms = use_true ? m.heading_true_ms : m.heading_mag_ms;
    hd *= RAD2DEG;
    if (hd < 0) hd += 360.0;
    if (hd >= 360.0) hd -= 360.0;
    s->heading_deg = (float)hd;
    s->heading_updated_ms = hms;
    s->heading_timeout_ms = T_HEADING;

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
    s->anchor_armed = m.anchor_on;
    s->anchor_radius_m = (m.anchor_on && m.anchor_max_m > 0) ? (uint16_t)lroundf(m.anchor_max_m) : s_radius_pref;
    s->anchor_dist_m = m.anchor_curr_m;
    double br = m.anchor_bearing_rad * RAD2DEG;
    if (br < 0) br += 360.0;
    s->anchor_bearing_deg = (float)br;
    s->anchor_updated_ms = m.anchor_on ? m.anchor_ms : now;
    s->anchor_timeout_ms = m.anchor_on ? T_ANCHOR : 0;

    /* Alarm: the plugin's notification, or an injected one (bench). */
    marine_alarm_type_t alarm = MARINE_ALARM_NONE;
    if (!strcmp(m.alarm_state, "alarm") || !strcmp(m.alarm_state, "emergency")) alarm = MARINE_ALARM_ANCHOR_DRAG;
    if (s_alarm_inject) alarm = (marine_alarm_type_t)s_alarm_inject;
    if (alarm == MARINE_ALARM_NONE) s_ack_latch = false;
    s->active_alarm = alarm;
    s->alarm_acknowledged = alarm != MARINE_ALARM_NONE && s_ack_latch;

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
    s->hid_profile = HID_PROFILE_CHARTPLOTTER;
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

static void put_cb(const char *request_id, const char *state, int status_code, const char *message, void *arg)
{
    (void)request_id; (void)arg;
    ESP_LOGI(TAG, "PUT result: %s %d %s", state ? state : "?", status_code, message ? message : "");
}

static bool online(void)
{
    knob_net_status_t ns;
    knob_net_get(&ns);
    return ns.phase == KNOB_NET_ONLINE;
}

/* One REST call with the knob's token. Logs the outcome, returns true on a 2xx reply. */
static bool rest(const char *method, const char *path, const char *body)
{
    espos_sk_http_opts_t o = { .timeout_ms = 5000, .max_body = 2048 };
    espos_sk_http_resp_t r = { 0 };
    esp_err_t e = !strcmp(method, "POST") ? espos_sk_http_post(path, body, &o, &r) : espos_sk_http_put(path, body, &o, &r);
    bool ok = e == ESP_OK && r.status >= 200 && r.status < 300;
    ESP_LOGI(TAG, "%s %s -> %s %d%s%.100s", method, path, esp_err_to_name(e), r.status, r.body && r.body[0] ? " " : "", r.body ? r.body : "");
    espos_sk_http_resp_free(&r);
    return ok;
}

static void light_set(int g, bool on)
{
    if (g < 0 || g >= N_LIGHTS) return;
    if (!online()) { ESP_LOGW(TAG, "light: not online, ignored"); return; }
    char v[16];
    if (s_lights[g].onoff) snprintf(v, sizeof(v), "%d", on ? 1 : 0);
    else snprintf(v, sizeof(v), "%.2f", on ? 1.0 : 0.0);
    esp_err_t e = espos_sk_put(s_lights[g].path, v, put_cb, NULL);
    ESP_LOGI(TAG, "light %d %s -> %s (%s)", g, s_lights[g].path, v, esp_err_to_name(e));
}

static void light_level(int g, int pct)
{
    if (g < 0 || g >= N_LIGHTS) return;
    if (!online()) { ESP_LOGW(TAG, "light: not online, ignored"); return; }
    char v[16];
    if (s_lights[g].onoff) snprintf(v, sizeof(v), "%d", pct > 0 ? 1 : 0);
    else snprintf(v, sizeof(v), "%.2f", pct / 100.0);
    esp_err_t e = espos_sk_put(s_lights[g].path, v, put_cb, NULL);
    ESP_LOGI(TAG, "light %d level %d -> %s (%s)", g, pct, v, esp_err_to_name(e));
}

static void ap_poll(void)
{
    espos_sk_http_opts_t o = { .timeout_ms = 3000, .max_body = 2048 };
    espos_sk_http_resp_t r = { 0 };
    char path[96];
    snprintf(path, sizeof(path), AP_BASE "%s", s_cfg.autopilot_id);
    esp_err_t e = espos_sk_http_get(path, &o, &r);
    if (e == ESP_OK && r.status == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.len);
        if (j) {
            const cJSON *eng = cJSON_GetObjectItem(j, "engaged");
            const cJSON *tg = cJSON_GetObjectItem(j, "target");
            portENTER_CRITICAL(&s_mux);
            s_m.ap_known = true;
            s_m.ap_engaged = cJSON_IsTrue(eng);
            s_m.ap_has_target = cJSON_IsNumber(tg);
            s_m.ap_target_rad = cJSON_IsNumber(tg) ? tg->valuedouble : 0;
            s_m.ap_ms = ms_now();
            portEXIT_CRITICAL(&s_mux);
            cJSON_Delete(j);
        }
    }
    espos_sk_http_resp_free(&r);
}

static void ap_action(const marine_ui_action_t *a)
{
    if (!s_live) { ESP_LOGW(TAG, "autopilot action %d ignored: live control is off (send l)", (int)a->action); return; }
    if (!online()) { ESP_LOGW(TAG, "autopilot action ignored: not online"); return; }
    char body[64], path[112];
    switch (a->action) {
    case MARINE_ACT_AP_ENGAGE:
        snprintf(path, sizeof(path), AP_BASE "%s/engage", s_cfg.autopilot_id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_STANDBY:
        snprintf(path, sizeof(path), AP_BASE "%s/disengage", s_cfg.autopilot_id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_SET_CTS:
        snprintf(body, sizeof(body), "{\"value\":%ld,\"units\":\"deg\"}", (long)a->value);
        snprintf(path, sizeof(path), AP_BASE "%s/target", s_cfg.autopilot_id);
        rest("PUT", path, body);
        break;
    default: break;
    }
    ap_poll(); /* show the new state without waiting for the next poll */
}

static void anchor_action(const marine_ui_action_t *a)
{
    if (a->action == MARINE_ACT_ANCHOR_SET_RADIUS) s_radius_pref = (uint16_t)a->value;
    if (!s_live) { ESP_LOGW(TAG, "anchor action %d ignored: live control is off (send l)", (int)a->action); return; }
    if (!online()) { ESP_LOGW(TAG, "anchor action ignored: not online"); return; }

    model_t m;
    portENTER_CRITICAL(&s_mux);
    m = s_m;
    portEXIT_CRITICAL(&s_mux);
    char body[200];
    bool pos_ok = m.pos_valid && ms_now() - m.pos_ms < 15000;

    switch (a->action) {
    case MARINE_ACT_ANCHOR_ARM_HERE:
        if (!pos_ok) { ESP_LOGW(TAG, "anchor arm: no fresh position"); return; }
        snprintf(body, sizeof(body), "{\"position\":{\"latitude\":%.7f,\"longitude\":%.7f},\"zone\":{\"type\":\"circle\",\"radius\":%u}}",
                 m.lat, m.lon, (unsigned)s_radius_pref);
        rest("POST", ANCHOR_PLUGIN "/dropAnchor", body);
        break;
    case MARINE_ACT_ANCHOR_SET_RADIUS:
        if (!m.anchor_on) break; /* only remembered for the next arm */
        snprintf(body, sizeof(body), "{\"zone\":{\"type\":\"circle\",\"radius\":%ld}}", (long)a->value);
        rest("POST", ANCHOR_PLUGIN "/setZone", body);
        break;
    case MARINE_ACT_ANCHOR_MOVE_DROP:
        if (!pos_ok) { ESP_LOGW(TAG, "anchor move: no fresh position"); return; }
        snprintf(body, sizeof(body), "{\"zone\":{\"type\":\"circle\",\"radius\":%u},\"position\":{\"latitude\":%.7f,\"longitude\":%.7f}}",
                 (unsigned)(m.anchor_max_m > 0 ? lroundf(m.anchor_max_m) : s_radius_pref), m.lat, m.lon);
        rest("POST", ANCHOR_PLUGIN "/setZone", body);
        break;
    case MARINE_ACT_ANCHOR_DISARM:
        rest("POST", ANCHOR_PLUGIN "/raiseAnchor", NULL);
        break;
    default: break;
    }
}

/* Acknowledge the anchor alarm through the server's Notifications API. The id comes from the notification list. */
static void alarm_ack(void)
{
    s_ack_latch = true; /* the screen reacts at once */
    if (!online() || s_alarm_inject) return; /* an injected bench alarm has no notification behind it */
    espos_sk_http_opts_t o = { .timeout_ms = 4000, .max_body = 8192 };
    espos_sk_http_resp_t r = { 0 };
    char id[64] = "";
    if (espos_sk_http_get("/signalk/v2/api/notifications", &o, &r) == ESP_OK && r.status == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.len);
        for (cJSON *c = j ? j->child : NULL; c; c = c->next) {
            const cJSON *path = cJSON_GetObjectItem(c, "path");
            if (cJSON_IsString(path) && !strcmp(path->valuestring, "notifications.navigation.anchor") && c->string) {
                strncpy(id, c->string, sizeof(id) - 1);
                break;
            }
        }
        cJSON_Delete(j);
    }
    espos_sk_http_resp_free(&r);
    if (!id[0]) { ESP_LOGW(TAG, "alarm ack: no anchor notification id found"); return; }
    char path[128];
    snprintf(path, sizeof(path), "/signalk/v2/api/notifications/%s/acknowledge", id);
    rest("POST", path, NULL);
}

static void handle_action(const marine_ui_action_t *a)
{
    ESP_LOGI(TAG, "action mode=%d act=%d value=%ld meta=%ld", (int)a->mode, (int)a->action, (long)a->value, (long)a->meta);
    switch (a->action) {
    case MARINE_ACT_DIMMER_SET_LEVEL: light_level((int)a->meta, (int)a->value); break;
    case MARINE_ACT_DIMMER_TOGGLE_POWER: {
        int g = (int)a->value;
        bool on = g >= 0 && g < N_LIGHTS && s_m.light[g] == 1;
        light_set(g, !on);
        break;
    }
    case MARINE_ACT_AP_ENGAGE:
    case MARINE_ACT_AP_STANDBY:
    case MARINE_ACT_AP_SET_CTS: ap_action(a); break;
    case MARINE_ACT_ANCHOR_ARM_HERE:
    case MARINE_ACT_ANCHOR_SET_RADIUS:
    case MARINE_ACT_ANCHOR_MOVE_DROP:
    case MARINE_ACT_ANCHOR_DISARM: anchor_action(a); break;
    case MARINE_ACT_ALARM_ACK: alarm_ack(); break;
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)a->value); break;
    case MARINE_ACT_ALARM_LED_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_SYS_FACTORY_RESET: ESP_LOGW(TAG, "factory reset requested (not wired yet)"); break;
    default: break; /* HID actions: knob_hid does not exist yet */
    }
}

static void worker_task(void *arg)
{
    uint32_t last_poll = 0;
    for (;;) {
        marine_ui_action_t a;
        if (xQueueReceive(s_q, &a, pdMS_TO_TICKS(100)) == pdTRUE) handle_action(&a);
        uint32_t now = ms_now();
        if (online() && now - last_poll >= AP_POLL_MS) {
            last_poll = now;
            ap_poll();
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------------------------- */

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
    if (s_q) xQueueSend(s_q, act, 0);
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
        s_live = !s_live;
        ESP_LOGW(TAG, "LIVE CONTROL %s: autopilot and anchor commands %s", s_live ? "ON" : "OFF", s_live ? "are SENT" : "are ignored");
        return;
    default:
        ESP_LOGI(TAG, "commands: a anchor alarm, g gps alarm, c clear, x stream down, s freeze heading, n night override, "
                      "d bypass network gating, l live control (now %s)", s_live ? "ON" : "off");
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
    strcpy(s_cfg.autopilot_id, "raymarineST"); /* defaults until knob_model_subscribe() loads the settings */
    s_cfg.heading_true = true;
    s_cfg.anchor_radius_m = 35;
    s_q = xQueueCreate(16, sizeof(marine_ui_action_t));
    xTaskCreate(model_task, "model", 6144, NULL, 2, NULL);
    xTaskCreate(worker_task, "skwork", 8192, NULL, 3, NULL);
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
    s_radius_pref = s_cfg.anchor_radius_m;
    subscribe_all();
    ESP_LOGI(TAG, "subscribed, %d light channel(s), autopilot '%s'", N_LIGHTS, s_cfg.autopilot_id);
}
