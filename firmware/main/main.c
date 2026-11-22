/* SPDX-License-Identifier: Apache-2.0
 * SignalK rotary knob firmware (bring-up stage): espOS plus the knob application. See ARCHITECTURE.md section 2.10. */
#include "board.h"
#include "demo_model.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_sk.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "knob_net.h"
#include "knob_power.h"
#include "lv_port.h"
#include "marine_ui.h"
#include "usb_dev.h"

static const char *TAG = "knob";

#define PATH "environment.inside.temperature"

static unsigned long ms(void) { return (unsigned long)(esp_timer_get_time() / 1000); }

void app_main(void)
{
    /* Display and input first, then the network (spike question 3: this order works). */
    ESP_LOGI(TAG, "hw bring-up start at %lu ms", ms());
    board_power_on();
    board_display_init(); /* raw test pattern, shown until LVGL draws its first frame */
    board_input_init();
    knob_power_init();
    usb_dev_init();
    usb_dev_set_rx_cb(demo_model_command);
    ESP_LOGI(TAG, "raw first frame done at %lu ms", ms());

    lv_port_init();
    lv_port_lock();
    marine_ui_init(demo_model_action_cb, NULL);
    lv_port_unlock();
    demo_model_start();
    input_init();
    ESP_LOGI(TAG, "marine_ui up at %lu ms", ms());

    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "espos_start returned at %lu ms", ms());
    knob_net_start();

    for (;;) {
        static int n;
        lv_port_diag_t ld;
        int phase;
        uint32_t loop_age, apply_age;
        lv_port_get_diag(&ld);
        demo_model_get_diag(&phase, &loop_age, &apply_age);
        if ((n++ % 5) == 0) {
            ESP_LOGI(TAG, "heap internal free %u min %u, psram free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "stack headroom bytes: lvgl %u input %u demo %u leds %u, main %u",
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("lvgl")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("input")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("demo")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("leds")),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        /* Stall watch: report once a second while the LVGL lock is held for long or the demo task stops looping. */
        if (ld.held_ms > 1500 || ld.in_handler_ms > 1500 || loop_age > 1500) {
            ESP_LOGW(TAG, "STALL lock holder=%s held=%lu ms, lvgl in handler %lu ms, demo phase=%d loop_age=%lu apply_age=%lu",
                     ld.holder, (unsigned long)ld.held_ms, (unsigned long)ld.in_handler_ms, phase,
                     (unsigned long)loop_age, (unsigned long)apply_age);
        }
        espos_sk_publish_number(PATH, 293.65);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
