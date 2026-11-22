/* SPDX-License-Identifier: Apache-2.0 */
#include "demo_model.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "knob_power.h"
#include "lv_port.h"
#include "usb_dev.h"

static const char *TAG = "demo";

static QueueHandle_t s_q;
static marine_ui_snapshot_t s_snap;
static volatile bool s_stream_down, s_heading_frozen;
static volatile int s_alarm_req = -1; /* -1 none pending, else marine_alarm_type_t */
static volatile int s_night_toggle;

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
    default:
        ESP_LOGI(TAG, "commands: a anchor alarm, g gps alarm, c clear, x stream down, s freeze heading, n night");
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
    uint32_t boot_start = (uint32_t)(esp_timer_get_time() / 1000);
    float h = s_snap.heading_deg;
    for (;;) {
        marine_ui_action_t a;
        while (xQueueReceive(s_q, &a, 0) == pdTRUE) apply_action(&a);

        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        s_snap.now_ms = now;

        /* Boot screen for 2.5 s, then operational. */
        uint32_t since = now - boot_start;
        if (since < 2500) {
            s_snap.system_screen = MARINE_SCREEN_BOOT;
            s_snap.boot_progress_pct = (uint8_t)(since * 100 / 2500);
        } else {
            s_snap.system_screen = MARINE_SCREEN_OPERATIONAL;
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

        lv_port_lock();
        marine_ui_apply_snapshot(&s_snap);
        lv_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void demo_model_start(void)
{
    s_q = xQueueCreate(16, sizeof(marine_ui_action_t));
    marine_ui_load_demo_snapshot(&s_snap);
    s_snap.ap_engaged = false; /* start in standby so the engage flow can be tried */
    s_snap.system_screen = MARINE_SCREEN_BOOT;
    s_snap.hid_profile = HID_PROFILE_CHARTPLOTTER;
    xTaskCreate(demo_task, "demo", 6144, NULL, 2, NULL);
}
