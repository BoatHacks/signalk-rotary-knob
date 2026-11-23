/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_api.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "espos_httpd.h"
#include "knob_actions.h"
#include "knob_logic.h"
#include "knob_model.h"
#include "usb_dev.h"

static const char *TAG = "knob_api";

static esp_err_t mode_get(httpd_req_t *req)
{
    char out[48];
    snprintf(out, sizeof(out), "{\"mode\":\"%s\"}", knob_mode_name((int)marine_ui_get_active_mode()));
    return espos_httpd_send_json(req, NULL, out);
}

static esp_err_t mode_put(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) return ESP_OK;
    char *body = NULL;
    size_t len = 0;
    esp_err_t err = espos_httpd_read_body(req, &body, &len);
    if (err != ESP_OK) return ESP_FAIL;
    int mode = -1;
    knob_mode_req_t r = knob_mode_request(body, usb_dev_host_present(), &mode);
    free(body);
    switch (r) {
    case KNOB_MODE_REQ_NO_HOST:
        return espos_httpd_send_error(req, "409 Conflict", "no_usb_host", "the USB-HID mode needs a USB host on the cable");
    case KNOB_MODE_REQ_BAD:
        return espos_httpd_send_error(req, "400 Bad Request", "bad_mode",
                                      "expected {\"mode\":\"autopilot\"|\"lights\"|\"anchor\"|\"hid\"}");
    case KNOB_MODE_REQ_OK:
    default:
        break;
    }
    if (!knob_model_mode_enabled(mode)) {
        return espos_httpd_send_error(req, "409 Conflict", "mode_hidden", "this mode is switched off in the knob's settings");
    }
    knob_model_set_mode(mode);
    ESP_LOGI(TAG, "mode %s from the API", knob_mode_name(mode));
    return mode_get(req);
}

static esp_err_t status_get(httpd_req_t *req)
{
    knob_model_status_t st;
    knob_model_get_status(&st);
    static const char *const alarms[] = { "none", "anchor_drag", "gps_lost" };
    const char *alarm = (st.alarm >= 0 && st.alarm < 3) ? alarms[st.alarm] : "unknown";
    const esp_app_desc_t *app = esp_app_get_description();
    char out[256];
    snprintf(out, sizeof(out),
             "{\"mode\":\"%s\",\"link\":\"%s\",\"allow_control\":%s,\"demo\":%s,\"alarm\":\"%s\",\"firmware\":\"%s\"}",
             knob_mode_name((int)marine_ui_get_active_mode()), st.link, knob_actions_is_live() ? "true" : "false", st.demo ? "true" : "false", alarm,
             app ? app->version : "");
    return espos_httpd_send_json(req, NULL, out);
}

void knob_api_start(void)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/knob/mode", .method = HTTP_GET, .handler = mode_get },
        { .uri = "/api/v1/knob/mode", .method = HTTP_PUT, .handler = mode_put },
        { .uri = "/api/v1/knob/status", .method = HTTP_GET, .handler = status_get },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = espos_httpd_register(&uris[i]);
        if (err != ESP_OK) ESP_LOGW(TAG, "%s not registered: %s", uris[i].uri, esp_err_to_name(err));
    }
}
