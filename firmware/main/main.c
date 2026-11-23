/* SPDX-License-Identifier: Apache-2.0
 * SignalK rotary knob firmware (bring-up stage): espOS plus the knob application. See ARCHITECTURE.md section 2.10. */
#include "board.h"
#include "knob_model.h"
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

/* Internal RAM after each boot step: free now and the low-water mark since boot. espOS reboots below 12 KB free, so the step
 * that takes the low-water mark down is the one to look at. */
static volatile unsigned s_alloc_fail_n, s_alloc_fail_size, s_alloc_fail_ms;
static volatile uint32_t s_alloc_fail_caps;
static const char *volatile s_alloc_fail_fn;

typedef struct { const char *step; unsigned long ms; unsigned free, min, largest; } heap_rec_t;
static heap_rec_t s_heap_rec[24];
static int s_heap_n;

static void heap_step(const char *step)
{
    heap_rec_t r = { step, ms(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) };
    if (s_heap_n < (int)(sizeof(s_heap_rec) / sizeof(s_heap_rec[0]))) s_heap_rec[s_heap_n++] = r;
    ESP_LOGI(TAG, "heap after %s at %lu ms: internal free %u min %u largest %u", r.step, r.ms, r.free, r.min, r.largest);
}

/* The USB serial port comes up after the first steps, so their lines are lost. Print the whole record again later. */
static void heap_report(void)
{
    for (int i = 0; i < s_heap_n; i++) {
        ESP_LOGI(TAG, "heap step %-18s %6lu ms  free %6u  min %6u  largest %6u", s_heap_rec[i].step, s_heap_rec[i].ms,
                 s_heap_rec[i].free, s_heap_rec[i].min, s_heap_rec[i].largest);
    }
    if (s_alloc_fail_n) {
        ESP_LOGW(TAG, "failed allocations: %u, last %u bytes caps 0x%lx in %s at %u ms", s_alloc_fail_n, s_alloc_fail_size,
                 (unsigned long)s_alloc_fail_caps, s_alloc_fail_fn ? s_alloc_fail_fn : "?", s_alloc_fail_ms);
    }
}

/* Failed allocations are counted and the last one remembered, then reported with the heap steps (the callback may run in any
 * context, so it only stores). */

static void alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    s_alloc_fail_n++;
    s_alloc_fail_size = (unsigned)size;
    s_alloc_fail_caps = caps;
    s_alloc_fail_fn = function_name;
    s_alloc_fail_ms = (unsigned)ms();
}

void app_main(void)
{
    heap_caps_register_failed_alloc_callback(alloc_failed);
    /* Display and input first, then the network (spike question 3: this order works). */
    ESP_LOGI(TAG, "hw bring-up start at %lu ms", ms());
    heap_step("start");
    board_power_on();
    board_display_init(); /* raw test pattern, shown until LVGL draws its first frame */
    board_input_init();
    knob_power_init();
    usb_dev_init();
    usb_dev_set_rx_cb(knob_model_command);
    ESP_LOGI(TAG, "raw first frame done at %lu ms", ms());
    heap_step("board, power, usb");

    lv_port_init();
    heap_step("lv_port");
    lv_port_lock();
    marine_ui_init(knob_model_action_cb, NULL);
    lv_port_unlock();
    heap_step("marine_ui");
    knob_model_start();
    input_init();
    heap_step("model, input");
    ESP_LOGI(TAG, "marine_ui up at %lu ms", ms());

    ESP_ERROR_CHECK(espos_start(NULL));
    ESP_LOGI(TAG, "espos_start returned at %lu ms", ms());
    heap_step("espos_start");
    knob_model_restore_mode();
    knob_net_start();
    knob_model_subscribe();
    heap_step("net, subscribe");

    size_t last_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    for (;;) {
        static int n;
        lv_port_diag_t ld;
        int phase;
        uint32_t loop_age, apply_age;
        lv_port_get_diag(&ld);
        knob_model_get_diag(&phase, &loop_age, &apply_age);
        /* Report every new internal RAM low with the time, so it can be matched with the boot phase in the log. */
        size_t min_now = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        if (min_now < last_min) {
            last_min = min_now;
            heap_step("new low");
        }
        if (n == 20 || n == 60) heap_report();
        if ((n++ % 5) == 0) {
            ESP_LOGI(TAG, "heap internal free %u min %u, psram free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI(TAG, "stack headroom bytes: lvgl %u input %u model %u skwork %u leds %u, main %u",
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("lvgl")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("input")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("model")),
                     (unsigned)uxTaskGetStackHighWaterMark(xTaskGetHandle("skwork")),
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
