/* SPDX-License-Identifier: Apache-2.0
 * LED identification spike: lights the five WS2812s in a fixed pattern so their positions on the ring can be mapped.
 *   LED index 0 (first on the data line) white, 1 green, 2 blue, 3 red, 4 off.
 * Nothing else runs: no display, no Wi-Fi, no espOS. The pattern is sent again once a second. */
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#define PIN_RAIL_1   1   /* same as the knob's board.h: LCD and touch power */
#define PIN_RAIL_2   2   /* 5 V on the connectors */
#define PIN_LED_DATA 48
#define LED_COUNT    5
#define LV           64  /* a quarter of full brightness, as in the knob firmware */

static const char *TAG = "led_id";

void app_main(void)
{
    gpio_config_t out = { .pin_bit_mask = (1ULL << PIN_RAIL_1) | (1ULL << PIN_RAIL_2), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_RAIL_1, 1);
    gpio_set_level(PIN_RAIL_2, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    led_strip_handle_t strip;
    led_strip_config_t cfg = {
        .strip_gpio_num = PIN_LED_DATA,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 144,   /* the whole frame (5 x 24 bits + reset) fits, no refill during a send */
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &strip));

    static const struct { uint8_t r, g, b; const char *name; } pat[LED_COUNT] = {
        { LV, LV, LV, "white" }, { 0, LV, 0, "green" }, { 0, 0, LV, "blue" }, { LV, 0, 0, "red" }, { 0, 0, 0, "off" },
    };
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(strip, i, pat[i].r, pat[i].g, pat[i].b);
        ESP_LOGI(TAG, "LED %d (index %d on the data line): %s", i + 1, i, pat[i].name);
    }
    for (;;) {
        led_strip_refresh(strip);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
