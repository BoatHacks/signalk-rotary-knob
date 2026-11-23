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
static volatile bool s_live;            /* LIVE CONTROL: autopilot and anchor commands are sent only while set */
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
    char path[96];
    snprintf(path, sizeof(path), AP_BASE "%s", s_cfg->autopilot_id);
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

static void ap_action(const marine_ui_action_t *a)
{
    if (!s_live) { ESP_LOGW(TAG, "autopilot action %d ignored: live control is off (send l)", (int)a->action); return; }
    if (!online()) { ESP_LOGW(TAG, "autopilot action ignored: not online"); return; }
    char body[64], path[112];
    switch (a->action) {
    case MARINE_ACT_AP_ENGAGE:
        snprintf(path, sizeof(path), AP_BASE "%s/engage", s_cfg->autopilot_id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_STANDBY:
        snprintf(path, sizeof(path), AP_BASE "%s/disengage", s_cfg->autopilot_id);
        rest("POST", path, NULL);
        break;
    case MARINE_ACT_AP_SET_CTS:
        snprintf(body, sizeof(body), "{\"value\":%ld,\"units\":\"deg\"}", (long)a->value);
        snprintf(path, sizeof(path), AP_BASE "%s/target", s_cfg->autopilot_id);
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
    if (!s_live) { ESP_LOGW(TAG, "anchor action %d ignored: live control is off (send l)", (int)a->action); report(MARINE_RESULT_LIVE_OFF); return; }
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
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)a->value); break;
    case MARINE_ACT_ALARM_LED_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE: knob_power_led_alarm(a->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF); break;
    case MARINE_ACT_SYS_FACTORY_RESET: ESP_LOGW(TAG, "factory reset requested (not wired yet)"); break;
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
        if (online() && now - last_poll >= AP_POLL_MS) {
            last_poll = now;
            ap_poll();
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

void knob_actions_set_live(bool live)
{
    s_live = live;
    ESP_LOGW(TAG, "LIVE CONTROL %s: autopilot and anchor commands %s", live ? "ON" : "OFF", live ? "are SENT" : "are ignored");
}

bool knob_actions_is_live(void) { return s_live; }

uint16_t knob_actions_anchor_radius(void) { return s_radius_pref; }

void knob_actions_set_anchor_radius(uint16_t m) { s_radius_pref = m; }
