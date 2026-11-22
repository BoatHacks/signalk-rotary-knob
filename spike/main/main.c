/* Spike: espOS + CrowPanel 1.28 hardware bring-up with an LVGL test screen. Throwaway code. */
#include "app_ui.h"
#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_sk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_port.h"

static const char *TAG = "knob_spike";

#define PATH "environment.inside.temperature"

void app_main(void)
{
    /* Display and input first, as the cockpit firmware does, then the network. */
    ESP_LOGI(TAG, "hw bring-up start at %lld ms", esp_timer_get_time() / 1000);
    board_power_on();
    board_display_init(); /* raw test pattern, shown until LVGL draws its first frame */
    board_input_init();
    ESP_LOGI(TAG, "raw first frame done at %lld ms", esp_timer_get_time() / 1000);

    lv_port_init();
    app_ui_init();
    ESP_LOGI(TAG, "LVGL screen up at %lld ms", esp_timer_get_time() / 1000);

    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "espos_start returned at %lld ms", esp_timer_get_time() / 1000);

    for (;;) {
        espos_sk_publish_number(PATH, 293.65);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
