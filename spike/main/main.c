/* Spike: espOS + raw CrowPanel 1.28 hardware bring-up. Throwaway code. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_sk.h"
#include "board.h"

static const char *TAG = "knob_spike";

#define PATH "environment.inside.temperature"

/* Logs encoder/button/touch changes and moves the marker on the display. */
static void input_task(void *arg)
{
    int last = 0x7fffffff, last_step = 0;
    bool last_btn = false, last_touch = false;
    for (;;) {
        int c = board_encoder_count();
        bool btn = board_button_pressed();
        uint16_t tx = 0, ty = 0;
        bool touch = board_touch_read(&tx, &ty);
        int step = c / 4; /* 4 quadrature edges per step; real detent count still to be measured */
        if (c != last || btn != last_btn) {
            ESP_LOGI(TAG, "encoder edges=%d steps=%d btn=%d", c, step, btn);
            board_display_text_counter(step, btn);
            last = c; last_btn = btn; last_step = step;
        }
        if (touch && !last_touch) ESP_LOGI(TAG, "touch down x=%u y=%u", tx, ty);
        if (!touch && last_touch) ESP_LOGI(TAG, "touch up");
        last_touch = touch;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    (void)last_step;
}

void app_main(void)
{
    /* Display and input first, as the cockpit firmware does, then the network. */
    ESP_LOGI(TAG, "hw bring-up start at %lld ms", esp_timer_get_time() / 1000);
    board_power_on();
    board_display_init();
    board_input_init();
    board_display_text_counter(0, false);
    ESP_LOGI(TAG, "first frame done at %lld ms", esp_timer_get_time() / 1000);
    xTaskCreate(input_task, "input", 4096, NULL, 5, NULL);

    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "espos_start returned at %lld ms", esp_timer_get_time() / 1000);

    for (;;) {
        espos_sk_publish_number(PATH, 293.65);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
