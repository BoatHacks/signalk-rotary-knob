/* SPDX-License-Identifier: Apache-2.0
 * LVGL 9.3 port for the CrowPanel 1.28": display only. Input does not go through LVGL indevs: the input component feeds
 * the semantic classifier in marine_ui directly. */
#include "lv_port.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lv_port";

#define DRAW_BUF_ROWS 40 /* partial render: 240 x 40 x 2 bytes = 19 KB per buffer, two buffers */

static SemaphoreHandle_t s_lock;
static lv_display_t *s_disp;

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Runs in ISR context when the SPI transfer is done. */
static void flush_done(void *ctx) { lv_display_flush_ready((lv_display_t *)ctx); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h)); /* the panel wants big-endian RGB565 */
    board_display_draw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static void lvgl_task(void *arg)
{
    for (;;) {
        lv_port_lock();
        uint32_t wait = lv_timer_handler();
        lv_port_unlock();
        if (wait < 5) wait = 5;
        if (wait > 30) wait = 30;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

void lv_port_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void lv_port_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

void lv_port_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    assert(s_lock);

    lv_init();
    lv_tick_set_cb(tick_ms);

    s_disp = lv_display_create(BOARD_LCD_W, BOARD_LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    size_t buf_bytes = BOARD_LCD_W * DRAW_BUF_ROWS * sizeof(uint16_t);
    void *b1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *b2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    assert(b1 && b2);
    lv_display_set_buffers(s_disp, b1, b2, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    /* From here on the panel IO reports completion to LVGL instead of the blocking test-pattern path. */
    board_display_set_flush_cb(flush_done, s_disp);

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "LVGL %d.%d.%d up, %dx%d, 2 x %u byte draw buffers", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, BOARD_LCD_W, BOARD_LCD_H, (unsigned)buf_bytes);
}
