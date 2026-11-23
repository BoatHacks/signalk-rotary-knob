/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_power.h"

#include "knob_logic.h"

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "knob_power";

#define LED_PERIOD_MS 40
#define LED_MAX       64 /* WS2812s at full power are blinding on a dark helm, scale to a quarter */

static volatile knob_led_alarm_t s_alarm;
static volatile uint8_t s_state_r, s_state_g, s_state_b;
static volatile bool s_night;

/* Steering indicator kind, set by knob_power_apply_snapshot. At night only these are shown (steady colours are off). */
enum { STEER_NONE = 0, STEER_PORT, STEER_STBD, STEER_YELLOW, STEER_BLUE };
static volatile uint8_t s_steer;
static volatile uint32_t s_oncourse_since_ms; /* when the indicator last changed to yellow or blue */

void knob_power_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    board_backlight_set(percent);
}

void knob_power_led_alarm(knob_led_alarm_t mode) { s_alarm = mode; }

void knob_power_led_state(uint8_t r, uint8_t g, uint8_t b)
{
    s_state_r = r; s_state_g = g; s_state_b = b;
}

void knob_power_set_night(bool night) { s_night = night; }

static void led_task(void *arg)
{
    for (;;) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t r = 0, g = 0, b = 0;
        switch (s_alarm) {
        case KNOB_LED_ALARM_FAST:
            r = ((t / 150) & 1) ? LED_MAX : 0;
            break;
        case KNOB_LED_ALARM_SLOW: {
            uint32_t ph = t % 2000; /* triangle wave, 2 s period */
            uint32_t tri = ph < 1000 ? ph : 2000 - ph;
            r = (uint8_t)(LED_MAX * tri / 1000);
            break;
        }
        default:
            if (!s_night) {
                r = s_state_r; g = s_state_g; b = s_state_b;
            } else if (s_steer == STEER_PORT) {
                r = LED_MAX;                          /* night: red and green stay on while steering */
            } else if (s_steer == STEER_STBD) {
                g = LED_MAX;
            } else if (s_steer == STEER_YELLOW || s_steer == STEER_BLUE) {
                /* On course (yellow engaged, blue standby): shown for KNOB_LED_NIGHT_FADE_HOLD_MS after the change to
                 * it, then fades linearly to off over KNOB_LED_NIGHT_FADE_MS. */
                uint32_t el = t - s_oncourse_since_ms;
                uint32_t permille = knob_fade_permille(el, KNOB_LED_NIGHT_FADE_HOLD_MS, KNOB_LED_NIGHT_FADE_MS);
                if (s_steer == STEER_YELLOW) { r = (uint8_t)(64 * permille / 1000); g = (uint8_t)(40 * permille / 1000); }
                else b = (uint8_t)(64 * permille / 1000);
            }
            break;
        }
        for (int i = 0; i < BOARD_LED_COUNT; i++) board_led_set(i, r, g, b);
        board_led_show();
        vTaskDelay(pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}

void knob_power_init(void)
{
    board_led_init();
    xTaskCreate(led_task, "leds", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "LED task up");
}

/* --- Steady colour from the state ------------------------------------------------------------------------------ */
#include <math.h>

void knob_power_apply_snapshot(const marine_ui_snapshot_t *s, marine_ui_mode_t active_mode)
{
    static bool on_course;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t steer = STEER_NONE;

    if (active_mode == MARINE_MODE_AUTOPILOT) {
        /* Steering indicator on the autopilot screen, engaged or in standby. Standby uses the set target. */
        bool stale = knob_stale_by(s->now_ms, s->heading_updated_ms, s->heading_timeout_ms) ||
                     knob_stale_by(s->now_ms, s->cts_updated_ms, s->cts_timeout_ms);
        if (stale) {
            on_course = false;
            if (s->ap_engaged) { r = 40; g = 24; b = 0; } /* dim amber: no usable course error */
        } else {
            float err = knob_course_error(s->cts_deg, s->heading_deg); /* negative: turn to port */
            knob_steer_t k = knob_steer_step(err, &on_course, KNOB_LED_ON_COURSE_ENTER_DEG, KNOB_LED_ON_COURSE_LEAVE_DEG);
            if (k == KNOB_STEER_ON_COURSE && s->ap_engaged) { r = 64; g = 40; b = 0; steer = STEER_YELLOW; }
            else if (k == KNOB_STEER_ON_COURSE) { b = 64; steer = STEER_BLUE; }
            else if (k == KNOB_STEER_PORT) { r = 64; steer = STEER_PORT; }
            else { g = 64; steer = STEER_STBD; }
        }
    } else {
        on_course = false;
        if (s->ap_engaged) g = 40;
        else if (s->anchor_dropped) { g = 20; b = 40; }
    }
    bool oncourse_kind = steer == STEER_YELLOW || steer == STEER_BLUE;
    if (oncourse_kind && s_steer != steer) s_oncourse_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_steer = steer;
    knob_power_led_state(r, g, b);
}
