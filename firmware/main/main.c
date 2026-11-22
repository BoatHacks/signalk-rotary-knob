/* SPDX-License-Identifier: Apache-2.0
 * SignalK rotary knob firmware (bring-up stage): espOS plus the knob application. See ARCHITECTURE.md section 2.10. */
#include "board.h"
#include "demo_model.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_sk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
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

    for (;;) {
        espos_sk_publish_number(PATH, 293.65);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
