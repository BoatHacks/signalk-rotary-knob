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
static volatile int s_depth;            /* recursion depth, only touched by the holder */
static volatile uint32_t s_taken_ms;    /* when the holder took the lock, 0 when free */
static volatile uint32_t s_handler_ms;  /* when the LVGL task entered lv_timer_handler, 0 when outside */
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
        s_handler_ms = tick_ms() ? tick_ms() : 1;
        uint32_t wait = lv_timer_handler();
        s_handler_ms = 0;
        lv_port_unlock();
        if (wait < 5) wait = 5;
        if (wait > 30) wait = 30;
        /* The FreeRTOS tick is 10 ms (CONFIG_FREERTOS_HZ=100), so pdMS_TO_TICKS(5) is 0 and vTaskDelay(0) does not sleep.
         * With an animation running (the acknowledged-alarm ring) LVGL asks for 5 ms every time, so this task spun,
         * retook the lock straight after releasing it, and starved every lower priority task that needs the lock (the demo
         * model never applied another snapshot). Always sleep at least one tick. */
        TickType_t ticks = pdMS_TO_TICKS(wait);
        if (ticks < 1) ticks = 1;
        vTaskDelay(ticks);
    }
}

void lv_port_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (++s_depth == 1) s_taken_ms = tick_ms() ? tick_ms() : 1;
}

void lv_port_unlock(void)
{
    if (--s_depth == 0) s_taken_ms = 0;
    xSemaphoreGiveRecursive(s_lock);
}

void lv_port_get_diag(lv_port_diag_t *d)
{
    TaskHandle_t h = xSemaphoreGetMutexHolder(s_lock);
    uint32_t now = tick_ms(), taken = s_taken_ms, hs = s_handler_ms;
    d->holder = h ? pcTaskGetName(h) : "none";
    d->held_ms = (h && taken) ? now - taken : 0;
    d->in_handler_ms = hs ? now - hs : 0;
}

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

/* LVGL allocator on PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). marine_ui creates hundreds of small objects and styles. With
 * LVGL on the C heap they stay in internal RAM (allocations under 4 KB do), which starved espOS: its health watchdog saw
 * internal RAM below 12 KB and rebooted the board. The DMA draw buffers stay in internal RAM (heap_caps in lv_port_init). */
#include "esp_heap_caps.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT); /* internal RAM as a fallback */
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_cnt = info.free_blocks;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_cnt = info.allocated_blocks;
    mon->max_used = info.total_allocated_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100 * info.total_allocated_bytes / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
