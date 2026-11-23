/* SPDX-License-Identifier: Apache-2.0 */
#include "demo_model.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_net.h"
#include "knob_power.h"
#include "lv_port.h"
#include "usb_dev.h"

static const char *TAG = "demo";

static QueueHandle_t s_q;
static marine_ui_snapshot_t s_snap;
static volatile bool s_stream_down, s_heading_frozen;
static volatile int s_alarm_req = -1; /* -1 none pending, else marine_alarm_type_t */
static volatile int s_night_toggle;
static volatile bool s_bypass_net; /* bench command d: show the operational UI without waiting for Wi-Fi and SignalK */
static volatile int s_phase;
static volatile uint32_t s_loop_ms, s_apply_done_ms;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void demo_model_get_diag(int *phase, uint32_t *loop_age_ms, uint32_t *apply_age_ms)
{
    uint32_t n = ms_now();
    *phase = s_phase;
    *loop_age_ms = n - s_loop_ms;
    *apply_age_ms = n - s_apply_done_ms;
}

void demo_model_action_cb(const marine_ui_action_t *act, void *ctx)
{
    (void)ctx;
    if (s_q) xQueueSend(s_q, act, 0); /* called under the LVGL lock, never block */
}

void demo_model_command(char c)
{
    switch (c) {
    case 'a': s_alarm_req = MARINE_ALARM_ANCHOR_DRAG; break;
    case 'g': s_alarm_req = MARINE_ALARM_GPS_LOST; break;
    case 'c': s_alarm_req = MARINE_ALARM_NONE; break;
    case 'x': s_stream_down = !s_stream_down; break;
    case 's': s_heading_frozen = !s_heading_frozen; break;
    case 'n': s_night_toggle = 1; break;
    case 'd': s_bypass_net = !s_bypass_net; break;
    default:
        ESP_LOGI(TAG, "commands: a anchor alarm, g gps alarm, c clear, x stream down, s freeze heading, n night, d bypass network gating");
        return;
    }
    ESP_LOGI(TAG, "command %c", c);
}

static void apply_action(const marine_ui_action_t *a)
{
    ESP_LOGI(TAG, "action mode=%d act=%d value=%ld meta=%ld", (int)a->mode, (int)a->action, (long)a->value,
             (long)a->meta);
    switch (a->action) {
    case MARINE_ACT_AP_SET_CTS: s_snap.cts_deg = (float)a->value; break;
    case MARINE_ACT_AP_ENGAGE: s_snap.ap_engaged = true; break;
    case MARINE_ACT_AP_STANDBY: s_snap.ap_engaged = false; break;
    case MARINE_ACT_ANCHOR_SET_RADIUS: s_snap.anchor_radius_m = (uint16_t)a->value; break;
    case MARINE_ACT_ANCHOR_ARM_HERE: s_snap.anchor_armed = true; break;
    case MARINE_ACT_ANCHOR_MOVE_DROP: break;
    case MARINE_ACT_ANCHOR_DISARM: s_snap.anchor_armed = false; break;
    case MARINE_ACT_ALARM_ACK: s_snap.alarm_acknowledged = true; break;
    case MARINE_ACT_DIMMER_SET_LEVEL:
        if (a->meta >= 0 && a->meta < s_snap.dimmer_group_count) {
            s_snap.dimmer_groups[a->meta].level_pct = (uint8_t)a->value;
            s_snap.dimmer_groups[a->meta].on = a->value > 0;
        }
        break;
    case MARINE_ACT_DIMMER_TOGGLE_POWER:
        if (a->value >= 0 && a->value < s_snap.dimmer_group_count) {
            marine_dimmer_group_t *g = &s_snap.dimmer_groups[a->value];
            g->on = !g->on;
            if (g->on && g->level_pct == 0) g->level_pct = 50;
        }
        break;
    case MARINE_ACT_DIMMER_SET_PRESET: s_snap.rgbw_preset_idx = (uint8_t)a->value; break;
    case MARINE_ACT_POWER_SET_BACKLIGHT: knob_power_set_backlight((uint8_t)a->value); break;
    case MARINE_ACT_ALARM_LED_PULSE:
        knob_power_led_alarm(a->value ? KNOB_LED_ALARM_FAST : KNOB_LED_ALARM_OFF);
        break;
    case MARINE_ACT_ALARM_LED_SLOW_PULSE:
        knob_power_led_alarm(a->value ? KNOB_LED_ALARM_SLOW : KNOB_LED_ALARM_OFF);
        break;
    case MARINE_ACT_SYS_FACTORY_RESET: ESP_LOGW(TAG, "factory reset requested (ignored in the demo)"); break;
    default: break; /* HID actions: knob_hid does not exist yet */
    }
}

static void demo_task(void *arg)
{
    float h = s_snap.heading_deg;
    for (;;) {
        marine_ui_action_t a;
        s_loop_ms = ms_now();
        s_phase = 1;
        while (xQueueReceive(s_q, &a, 0) == pdTRUE) apply_action(&a);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_snap.now_ms = now;

        /* The system screen follows the real Wi-Fi and SignalK state from knob_net. The data behind the operational
         * screens is still simulated until knob_model exists. Bench command d bypasses the gating. */
        knob_net_status_t ns;
        knob_net_get(&ns);
        if (s_bypass_net) {
            s_snap.system_screen = MARINE_SCREEN_OPERATIONAL;
        } else {
            switch (ns.phase) {
            case KNOB_NET_PORTAL:
                s_snap.system_screen = MARINE_SCREEN_WIFI_PORTAL;
                strncpy(s_snap.setup_ap_ssid, ns.portal_ssid, sizeof(s_snap.setup_ap_ssid) - 1);
                break;
            case KNOB_NET_PAIRING:
                s_snap.system_screen = MARINE_SCREEN_PAIRING;
                strncpy(s_snap.device_name, ns.hostname, sizeof(s_snap.device_name) - 1);
                break;
            case KNOB_NET_FAILED:
                s_snap.system_screen = MARINE_SCREEN_CANNOT_CONNECT;
                break;
            case KNOB_NET_ONLINE:
                s_snap.system_screen = MARINE_SCREEN_OPERATIONAL;
                break;
            default: /* starting, Wi-Fi, server, token, link: the splash with the phase name */
                s_snap.system_screen = MARINE_SCREEN_BOOT;
                break;
            }
            strncpy(s_snap.boot_phase, ns.text, sizeof(s_snap.boot_phase) - 1);
            s_snap.boot_progress_pct = ns.progress;
        }

        if (s_night_toggle) {
            s_snap.night_palette = !s_snap.night_palette;
            knob_power_set_night(s_snap.night_palette);
            s_night_toggle = 0;
        }
        if (s_alarm_req >= 0) {
            s_snap.active_alarm = (marine_alarm_type_t)s_alarm_req;
            s_snap.alarm_acknowledged = false;
            s_alarm_req = -1;
        }

        /* Heading follows the target while engaged, otherwise drifts slowly. */
        if (!s_heading_frozen) {
            float t = s_snap.ap_engaged ? s_snap.cts_deg : h + 0.3f;
            float d = fmodf(t - h + 540.0f, 360.0f) - 180.0f;
            h += d * 0.05f;
            if (h < 0) h += 360.0f;
            if (h >= 360.0f) h -= 360.0f;
            s_snap.heading_deg = h;
            s_snap.heading_updated_ms = now;
            s_snap.rudder_deg = d * 0.1f;
        }
        s_snap.heading_timeout_ms = 5000;
        s_snap.anchor_dist_m = s_snap.anchor_armed ? 14.0f + 8.0f * sinf((float)now / 4000.0f) : 0.0f;
        s_snap.signalk_connected = !s_stream_down;
        if (!s_stream_down) s_snap.last_data_ms = now;
        s_snap.usb_host_enumerated = usb_dev_mounted();

        knob_power_apply_snapshot(&s_snap, marine_ui_get_active_mode());

        s_phase = 2;
        lv_port_lock();
        s_phase = 3;
        marine_ui_apply_snapshot(&s_snap);
        lv_port_unlock();
        s_apply_done_ms = ms_now();
        s_phase = 4;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void demo_model_start(void)
{
    s_q = xQueueCreate(16, sizeof(marine_ui_action_t));
    marine_ui_load_demo_snapshot(&s_snap);
    s_snap.ap_engaged = false; /* start in standby so the engage flow can be tried */
    /* The boat has relays, not dimmers: the first channel is an on/off one, the others stay level dimmers for testing. */
    strcpy(s_snap.dimmer_groups[0].name, "RELAY 1");
    s_snap.dimmer_groups[0].onoff_only = true;
    s_snap.dimmer_groups[0].level_pct = 100;
    s_snap.system_screen = MARINE_SCREEN_BOOT;
    s_snap.hid_profile = HID_PROFILE_CHARTPLOTTER;
    xTaskCreate(demo_task, "demo", 6144, NULL, 2, NULL);
}
