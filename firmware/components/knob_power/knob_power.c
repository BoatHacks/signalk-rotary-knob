/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_power.h"

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
            if (!s_night) { r = s_state_r; g = s_state_g; b = s_state_b; }
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
