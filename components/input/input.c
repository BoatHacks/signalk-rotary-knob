/* SPDX-License-Identifier: Apache-2.0 */
#include "input.h"

#include <stdbool.h>
#include <stdint.h>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#include "lv_port.h"
#include "marine_ui.h"

static const char *TAG = "input";

#define INPUT_PERIOD_MS 5
#define TICK_EVERY_MS   50

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static input_hook_t s_hook;
static volatile bool s_hook_on;

void input_set_hook(input_hook_t hook) { s_hook = hook; }
void input_hook_enable(bool on) { s_hook_on = on; }

static void input_task(void *arg)
{
    bool btn_prev = false, touch_prev = false;
    uint16_t tx = 0, ty = 0;
    uint32_t last_tick = now_ms();

    for (;;) {
        int steps = board_encoder_take_steps();
        bool btn = board_button_pressed();
        uint16_t x, y;
        bool touch = board_touch_read(&x, &y);
        uint32_t now = now_ms();
        bool tick = (now - last_tick) >= TICK_EVERY_MS;
        bool user = steps || btn != btn_prev || (touch && !touch_prev);
        bool hook = s_hook && s_hook_on;

        if (steps || btn != btn_prev || touch || touch_prev || tick || hook) {
            lv_port_lock();
            for (; steps > 0; steps--) marine_ui_feed_encoder_step(1);
            for (; steps < 0; steps++) marine_ui_feed_encoder_step(-1);
            if (btn != btn_prev) marine_ui_feed_button_edge(btn);
            if (touch) {
                tx = x;
                ty = y;
                marine_ui_feed_touch((int16_t)x, (int16_t)y, true);
            } else if (touch_prev) {
                marine_ui_feed_touch((int16_t)tx, (int16_t)ty, false);
            }
            if (hook) s_hook(now, user);   /* after the real inputs: real input pauses the automatic demo */
            if (tick) {
                marine_ui_tick(now);
                last_tick = now;
            }
            lv_port_unlock();
        }
        btn_prev = btn;
        touch_prev = touch;
        vTaskDelay(pdMS_TO_TICKS(INPUT_PERIOD_MS));
    }
}

void input_init(void)
{
    xTaskCreateWithCaps(input_task, "input", 6144, NULL, 6, NULL, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
    ESP_LOGI(TAG, "input task up");
}
