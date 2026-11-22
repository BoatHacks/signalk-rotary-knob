/* Spike: LVGL 9.3 port for the CrowPanel 1.28". Throwaway code. */
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
static lv_indev_t *s_enc;
static lv_indev_t *s_touch;
static lv_group_t *s_group;

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Runs in ISR context when the SPI transfer is done. */
static void flush_done(void *ctx) { lv_display_flush_ready((lv_display_t *)ctx); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int w = lv_area_get_width(area), h = lv_area_get_height(area);
    lv_draw_sw_rgb565_swap(px_map, (uint32_t)(w * h)); /* the panel wants big-endian RGB565 */
    board_display_draw(area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/* Encoder: steps since the last read plus the push button. Both activity types reset LVGL's inactivity timer
 * (lv_display_get_inactive_time), which is what the dimmer in app_ui.c relies on. */
static void encoder_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->enc_diff = (int16_t)board_encoder_take_steps();
    data->state = board_button_pressed() ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static lv_point_t last;
    uint16_t x, y;
    if (board_touch_read(&x, &y)) {
        last.x = x < BOARD_LCD_W ? x : BOARD_LCD_W - 1; /* mapping to screen axes is UNVERIFIED on the device */
        last.y = y < BOARD_LCD_H ? y : BOARD_LCD_H - 1;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    data->point = last;
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
lv_group_t *lv_port_group(void) { return s_group; }

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

    s_group = lv_group_create();
    lv_group_set_default(s_group);

    s_enc = lv_indev_create();
    lv_indev_set_type(s_enc, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(s_enc, encoder_read_cb);
    lv_indev_set_group(s_enc, s_group);

    if (board_touch_present()) {
        s_touch = lv_indev_create();
        lv_indev_set_type(s_touch, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch, touch_read_cb);
    } else {
        ESP_LOGW(TAG, "no touch controller, pointer indev not created");
    }

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    ESP_LOGI(TAG, "LVGL %d.%d.%d up, %dx%d, 2 x %u byte draw buffers, touch %s", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, BOARD_LCD_W, BOARD_LCD_H, (unsigned)buf_bytes, s_touch ? "on" : "off");
}
