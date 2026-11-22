/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "espos_sk.h"
#include "espos_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"

static const char *TAG = "knob_net";

#define POLL_MS 250

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static knob_net_status_t s_st = { .phase = KNOB_NET_STARTING, .text = "STARTING", .progress = 5 };

/* The token state is the string after the first "state": in espos_sk_status_json(), which starts with the token object.
 * Copies at most n-1 characters. */
static void token_state(char *out, size_t n)
{
    out[0] = '\0';
    char *json = NULL;
    if (espos_sk_status_json(&json) != ESP_OK || !json) return;
    const char *p = strstr(json, "\"state\":\"");
    if (p) {
        p += 9;
        size_t i = 0;
        while (p[i] && p[i] != '"' && i + 1 < n) { out[i] = p[i]; i++; }
        out[i] = '\0';
    }
    free(json);
}

static void set(knob_net_phase_t ph, const char *text, uint8_t progress, const knob_net_status_t *base)
{
    knob_net_status_t n = *base;
    n.phase = ph;
    n.progress = progress;
    strncpy(n.text, text, sizeof(n.text) - 1);
    n.text[sizeof(n.text) - 1] = '\0';
    portENTER_CRITICAL(&s_mux);
    s_st = n;
    portEXIT_CRITICAL(&s_mux);
}

static void poll_task(void *arg)
{
    knob_net_phase_t last = (knob_net_phase_t)-1;
    for (;;) {
        knob_net_status_t b = { 0 };
        espos_wifi_status_t w;
        if (espos_wifi_get_status(&w) != ESP_OK) {
            set(KNOB_NET_STARTING, "STARTING", 5, &b);
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
            continue;
        }
        strncpy(b.hostname, w.hostname, sizeof(b.hostname) - 1);
        strncpy(b.portal_ssid, w.portal_ssid, sizeof(b.portal_ssid) - 1);
        token_state(b.token_state, sizeof(b.token_state));
        const char *t = b.token_state;

        espos_sk_ws_status_t ws = { 0 };
        espos_sk_ws_get_status(&ws);

        switch (w.sm.state) {
        case ESPOS_WIFI_ST_DISABLED:
        case ESPOS_WIFI_ST_UNCONFIGURED:
            set(KNOB_NET_PORTAL, "SETUP", 15, &b);
            break;
        case ESPOS_WIFI_ST_CONNECTING:
        case ESPOS_WIFI_ST_OBTAINING_IP:
        case ESPOS_WIFI_ST_BACKOFF:
            set(KNOB_NET_WIFI, "WI-FI", 30, &b);
            break;
        case ESPOS_WIFI_ST_CONNECTED:
        default:
            if (ws.connected) {
                set(KNOB_NET_ONLINE, "ONLINE", 100, &b);
            } else if (!strcmp(t, "pending")) {
                set(KNOB_NET_PAIRING, "APPROVE", 70, &b);
            } else if (!strcmp(t, "denied") || !strcmp(t, "error") || !strcmp(t, "cert_error")) {
                set(KNOB_NET_FAILED, "NO SERVER", 55, &b);
            } else if (!strcmp(t, "verifying")) {
                set(KNOB_NET_TOKEN, "TOKEN", 80, &b);
            } else if (!strcmp(t, "approved") || !strcmp(t, "open")) {
                set(KNOB_NET_LINK, "SIGNALK", 90, &b);
            } else {
                set(KNOB_NET_SERVER, "SIGNALK", 55, &b); /* no server yet, or the request is about to be sent */
            }
            break;
        }
        if (s_st.phase != last) {
            ESP_LOGI(TAG, "phase %d %s (wifi sm %d, token '%s', stream %d)", (int)s_st.phase, s_st.text, (int)w.sm.state,
                     t, (int)ws.connected);
            last = s_st.phase;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void knob_net_get(knob_net_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

void knob_net_start(void)
{
    xTaskCreate(poll_task, "knob_net", 4096, NULL, 2, NULL);
}
