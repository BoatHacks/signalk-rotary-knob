/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_actions.h"

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
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_hid.h"
#include "knob_logic.h"
#include "knob_net.h"
#include "knob_power.h"

static const char *TAG = "knob_actions";

#define ANCHOR_PLUGIN "/plugins/hoekens-anchor-alarm"
#define AP_BASE "/signalk/v2/api/vessels/self/autopilots/"
#define AP_POLL_MS 2000

static const knob_cfg_t *s_cfg;
#define s_lights s_cfg->lights
#define N_LIGHTS s_cfg->n_lights

static const knob_actions_hooks_t *s_hooks;
static QueueHandle_t s_q;
static volatile bool s_allow;           /* the stored allow control setting */
static volatile int s_override = -1;    /* serial bench override for this session: -1 none, 0 off, 1 on */
static volatile bool s_demo;            /* demo mode: nothing goes to SignalK, knob_model applies those actions itself */
static volatile uint16_t s_radius_pref = 35;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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
    char path[96], id[32];
    s_hooks->autopilot_id(id, sizeof(id));
    snprintf(path, sizeof(path), AP_BASE "%s", id);
    esp_err_t e = espos_sk_http_get(path, &o, &r);
    if (e == ESP_OK && r.status == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.len);
        if (j) {
            const cJSON *eng = cJSON_GetObjectItem(j, "engaged");
            const cJSON *tg = cJSON_GetObjectItem(j, "target");
            s_hooks->set_autopilot(cJSON_IsTrue(eng), cJSON_IsNumber(tg), cJSON_IsNumber(tg) ? tg->valuedouble : 0);
            cJSON_Delete(j);
        }
    }
    espos_sk_http_resp_free(&r);
}

/* ---- Volume (signalk-jukebox): volume through SignalK PUT (ratio), controls as a PUT 1 then 0, zone names over REST ---- */

#define JUKEBOX "entertainment.jukebox."

static void vol_set(int target, int pct)
{
    if (!online()) { ESP_LOGW(TAG, "volume: not online, ignored"); return; }
    knob_actions_view_t v;
    s_hooks->get_view(&v);
    char path[96], val[16];
    if (target == 0) snprintf(path, sizeof(path), JUKEBOX "playback.volume");
    else if (target >= 1 && target <= 4 && v.zone_id[target - 1][0]) snprintf(path, sizeof(path), JUKEBOX "zones.%s.volume", v.zone_id[target - 1]);
    else return;
    snprintf(val, sizeof(val), "%.2f", (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100.0);
    esp_err_t e = espos_sk_put(path, val, put_cb, NULL);
    ESP_LOGI(TAG, "volume %s -> %s (%s)", path, val, esp_err_to_name(e));
}

static void vol_control(int c)
{
    static const char *const names[] = { "play", "pause", "next", "previous" };
    if (c < 0 || c > 3) return;
    if (!online()) { ESP_LOGW(TAG, "jukebox: not online, ignored"); return; }
    char path[64];
    snprintf(path, sizeof(path), JUKEBOX "playback.controls.%s", names[c]);
    espos_sk_put(path, "1", put_cb, NULL);
    vTaskDelay(pdMS_TO_TICKS(300)); /* a momentary control: 1, then back to 0 */
    espos_sk_put(path, "0", put_cb, NULL);
    ESP_LOGI(TAG, "jukebox %s", names[c]);
}

/* GET /plugins/signalk-jukebox/api/zones: [{id, name, ...}]. Only the names are used, volumes come through the stream. */
static void zone_names_fetch(void)
{
    espos_sk_http_opts_t o = { .timeout_ms = 3000, .max_body = 4096 };
    espos_sk_http_resp_t r = { 0 };
    esp_err_t e = espos_sk_http_get("/plugins/signalk-jukebox/api/zones", &o, &r);
    if (e == ESP_OK && r.status == 200 && r.body) {
        cJSON *j = cJSON_ParseWithLength(r.body, r.len);
        if (cJSON_IsArray(j)) {
            const cJSON *z;
            cJSON_ArrayForEach(z, j) {
                const cJSON *id = cJSON_GetObjectItem(z, "id"), *nm = cJSON_GetObjectItem(z, "name");
                if (cJSON_IsString(id) && cJSON_IsString(nm) && s_hooks->zone_name) s_hooks->zone_name(id->valuestring, nm->valuestring);
            }
        }
        cJSON_Delete(j);
    } else {
        ESP_LOGW(TAG, "jukebox zone names: %s, status %d", esp_err_to_name(e), r.status);
    }
    espos_sk_http_resp_free(&r);
}

static void ap_action(const marine_ui_action_t *a)
{
    if (!knob_actions_is_live()) { ESP_LOGW(TAG, "autopilot action %d ignored: allow control is off", (int)a->action); return; }
    if (!online()) { ESP_LOGW(TAG, "autopilot action ignored: not online"); return; }
    char body[64], path[112], id[32];
    s_hooks->autopilot_id(id, sizeof(id));
    switch (a->action) {
    case MARINE_ACT_AP_ENGAGE:
        snprintf(path, sizeof(path), AP_BASE "%s/engage", id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_STANDBY:
        snprintf(path, sizeof(path), AP_BASE "%s/disengage", id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_SET_CTS:
        snprintf(body, sizeof(body), "{\"value\":%ld,\"units\":\"deg\"}", (long)a->value);
        snprintf(path, sizeof(path), AP_BASE "%s/target", id);
        rest("PUT", path, body);
        break;
    default: break;
    }
    ap_poll(); /* show the new state without waiting for the next poll */
}

/* Tell the screen what became of an anchor action. It shows a pending screen after the press and ends it with this. */
static void report(marine_action_result_t code)
{
    if (s_hooks->action_result) s_hooks->action_result((uint8_t)code);
}

static void anchor_action(const marine_ui_action_t *a)
{
    knob_actions_view_t m;
    s_hooks->get_view(&m);
    if (a->action == MARINE_ACT_ANCHOR_SET_RADIUS) {
        s_radius_pref = (uint16_t)a->value;
        if (!m.anchor_on) return; /* only remembered for the next drop, no server call and nothing to report */
    }
    if (!knob_actions_is_live()) { ESP_LOGW(TAG, "anchor action %d ignored: allow control is off", (int)a->action); report(MARINE_RESULT_LIVE_OFF); return; }
    if (!online()) { ESP_LOGW(TAG, "anchor action ignored: not online"); report(MARINE_RESULT_NO_LINK); return; }

    char body[200];
    bool pos_ok = m.pos_valid && ms_now() - m.pos_ms < 15000;
    bool ok = false;

    switch (a->action) {
    case MARINE_ACT_ANCHOR_DROP:
        if (!pos_ok) { ESP_LOGW(TAG, "anchor drop: no fresh position"); report(MARINE_RESULT_NO_POSITION); return; }
        snprintf(body, sizeof(body), "{\"position\":{\"latitude\":%.7f,\"longitude\":%.7f},\"zone\":{\"type\":\"circle\",\"radius\":%u}}",
                 m.lat, m.lon, (unsigned)s_radius_pref);
        ok = rest("POST", ANCHOR_PLUGIN "/dropAnchor", body);
        break;
    case MARINE_ACT_ANCHOR_SET_RADIUS:
        snprintf(body, sizeof(body), "{\"zone\":{\"type\":\"circle\",\"radius\":%ld}}", (long)a->value);
        ok = rest("POST", ANCHOR_PLUGIN "/setZone", body);
        break;
    case MARINE_ACT_ANCHOR_MOVE_DROP:
        if (!pos_ok) { ESP_LOGW(TAG, "anchor move: no fresh position"); report(MARINE_RESULT_NO_POSITION); return; }
        snprintf(body, sizeof(body), "{\"zone\":{\"type\":\"circle\",\"radius\":%u},\"position\":{\"latitude\":%.7f,\"longitude\":%.7f}}",
                 (unsigned)(m.anchor_max_m > 0 ? lroundf(m.anchor_max_m) : s_radius_pref), m.lat, m.lon);
        ok = rest("POST", ANCHOR_PLUGIN "/setZone", body);
        break;
    case MARINE_ACT_ANCHOR_RAISE:
        ok = rest("POST", ANCHOR_PLUGIN "/raiseAnchor", NULL);
        break;
    default: return;
    }
    report(ok ? MARINE_RESULT_OK : MARINE_RESULT_FAILED);
}

/* Acknowledge the anchor alarm through the server's Notifications API. The id comes from the notification list. */
static void alarm_ack(void)
{
    s_hooks->ack_latch(); /* the screen reacts at once */
    if (!online() || s_hooks->alarm_injected()) return; /* an injected bench alarm has no notification behind it */
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
    if (s_demo && a->action != MARINE_ACT_HID_TURN && a->action != MARINE_ACT_HID_PRESS) {
        ESP_LOGW(TAG, "demo mode: action %d not sent", (int)a->action);
        return;
    }
    switch (a->action) {
    case MARINE_ACT_DIMMER_SET_LEVEL: light_level((int)a->meta, (int)a->value); break;
    case MARINE_ACT_DIMMER_TOGGLE_POWER: {
        int g = (int)a->value;
        knob_actions_view_t v;
        s_hooks->get_view(&v);
        bool on = g >= 0 && g < N_LIGHTS && v.light[g] == 1;
        light_set(g, !on);
        break;
    }
    case MARINE_ACT_AP_ENGAGE:
    case MARINE_ACT_AP_STANDBY:
    case MARINE_ACT_AP_SET_CTS: ap_action(a); break;
    case MARINE_ACT_ANCHOR_DROP:
    case MARINE_ACT_ANCHOR_SET_RADIUS:
    case MARINE_ACT_ANCHOR_MOVE_DROP:
    case MARINE_ACT_ANCHOR_RAISE: anchor_action(a); break;
    case MARINE_ACT_ALARM_ACK: alarm_ack(); break;
    case MARINE_ACT_VOL_SET: vol_set((int)a->meta, (int)a->value); break;
    case MARINE_ACT_VOL_CONTROL: vol_control((int)a->value); break;
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)a->value); break;
    case MARINE_ACT_ALARM_LED_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_HID_TURN: knob_hid_turn((int)a->value); break;
    case MARINE_ACT_HID_PRESS: knob_hid_press(); break;
    case MARINE_ACT_HID_NEXT_PROFILE: knob_hid_next_profile(); break;
    default: break;
    }
}

static void worker_task(void *arg)
{
    uint32_t last_poll = 0;
    for (;;) {
        marine_ui_action_t a;
        if (xQueueReceive(s_q, &a, pdMS_TO_TICKS(100)) == pdTRUE) handle_action(&a);
        uint32_t now = ms_now();
        if (!s_demo && online() && now - last_poll >= AP_POLL_MS) {
            last_poll = now;
            ap_poll();
            knob_actions_view_t v;
            s_hooks->get_view(&v);
            static uint32_t last_names;
            if (v.zone_names_wanted && (last_names == 0 || now - last_names >= 30000)) { /* at most every 30 s while a zone has no name */
                last_names = now;
                zone_names_fetch();
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------------------------------------------- */

void knob_actions_start(const knob_cfg_t *cfg, const knob_actions_hooks_t *hooks)
{
    s_cfg = cfg;
    s_hooks = hooks;
    s_q = xQueueCreate(16, sizeof(marine_ui_action_t));
    knob_hid_start();
    xTaskCreateWithCaps(worker_task, "skwork", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
}

void knob_actions_submit(const marine_ui_action_t *act)
{
    if (s_q) xQueueSend(s_q, act, 0); /* called under the LVGL lock, never block */
}

static void log_control(const char *source)
{
    bool on = knob_actions_is_live();
    ESP_LOGW(TAG, "ALLOW CONTROL %s (%s%s): autopilot and anchor commands %s", on ? "ON" : "OFF", source,
             s_override >= 0 ? ", serial override for this session" : "", on ? "are SENT" : "are ignored");
}

void knob_actions_set_allow_control(bool on, const char *source)
{
    if (on == s_allow && strcmp(source, "boot") != 0) return;
    s_allow = on;
    log_control(source);
}

void knob_actions_toggle_override(void)
{
    s_override = knob_actions_is_live() ? 0 : 1;
    log_control("serial");
}

bool knob_actions_is_live(void) { return knob_allow_control(s_allow, s_override); }

void knob_actions_set_demo(bool on) { s_demo = on; }

uint16_t knob_actions_anchor_radius(void) { return s_radius_pref; }

void knob_actions_set_anchor_radius(uint16_t m) { s_radius_pref = m; }
