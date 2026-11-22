/* Spike: raw hardware bring-up (power, GC9A01, backlight, PCNT encoder, touch). Throwaway code. */
#include "board.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define LCD_HOST      SPI2_HOST
#define LCD_PCLK_HZ   (40 * 1000 * 1000)
#define CHUNK_ROWS    16
#define TP_ADDR       0x15

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_flush_done;
static uint16_t *s_line;            /* DMA-capable, CHUNK_ROWS rows */
static pcnt_unit_handle_t s_pcnt;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_tp;
static bool s_tp_ok;
static board_flush_done_cb_t s_flush_cb;
static void *s_flush_ctx;
static int s_enc_taken;     /* edges already handed out as steps */

/* RGB565, byte-swapped for the SPI wire. */
#define C565(r, g, b) __builtin_bswap16((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define COL_BLACK  C565(0, 0, 0)
#define COL_WHITE  C565(255, 255, 255)
#define COL_RED    C565(255, 0, 0)
#define COL_GREEN  C565(0, 255, 0)
#define COL_BLUE   C565(0, 0, 255)
#define COL_YELLOW C565(255, 255, 0)

void board_power_on(void)
{
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << BOARD_PIN_RAIL_1) | (1ULL << BOARD_PIN_RAIL_2) | (1ULL << BOARD_PIN_PWR_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(BOARD_PIN_RAIL_1, 1);
    gpio_set_level(BOARD_PIN_RAIL_2, 1);
    gpio_set_level(BOARD_PIN_PWR_LED, 0); /* Elecrow drives this low at boot */
    vTaskDelay(pdMS_TO_TICKS(20));        /* let the LCD/touch rail settle */
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *ev, void *ctx)
{
    if (s_flush_cb) {
        s_flush_cb(s_flush_ctx);
        return false;
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &woken);
    return woken == pdTRUE;
}

static void draw_rows(int x0, int y0, int w, int h)
{
    esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x0 + w, y0 + h, s_line);
    xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500));
}

static void fill_rect(int x0, int y0, int w, int h, uint16_t color)
{
    for (int i = 0; i < w * CHUNK_ROWS && i < BOARD_LCD_W * CHUNK_ROWS; i++) s_line[i] = color;
    for (int y = y0; y < y0 + h; y += CHUNK_ROWS) {
        int rows = (y0 + h - y) < CHUNK_ROWS ? (y0 + h - y) : CHUNK_ROWS;
        draw_rows(x0, y, w, rows);
    }
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = BOARD_PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void board_backlight_set(uint8_t percent)
{
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (percent * 255) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void board_display_init(void)
{
    backlight_init();
    s_flush_done = xSemaphoreCreateBinary();
    s_line = heap_caps_malloc(BOARD_LCD_W * CHUNK_ROWS * sizeof(uint16_t), MALLOC_CAP_DMA);
    assert(s_line);

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_PIN_LCD_SCLK, .mosi_io_num = BOARD_PIN_LCD_MOSI, .miso_io_num = -1,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_LCD_W * CHUNK_ROWS * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_PIN_LCD_DC, .cs_gpio_num = BOARD_PIN_LCD_CS, .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 4,
        .on_color_trans_done = on_color_trans_done, .user_ctx = s_flush_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BOARD_PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true)); /* Elecrow: panelConfig.invert = true */
    /* Seen on the device: the image is mirrored left-right. Flip x. The touch x axis may need the same flip. */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));

    /* Orientation test pattern: top red, right green, bottom blue, left yellow, black centre. */
    fill_rect(0, 0, BOARD_LCD_W, BOARD_LCD_H, COL_BLACK);
    fill_rect(0, 0, BOARD_LCD_W, 40, COL_RED);
    fill_rect(200, 40, 40, 160, COL_GREEN);
    fill_rect(0, 200, BOARD_LCD_W, 40, COL_BLUE);
    fill_rect(0, 40, 40, 160, COL_YELLOW);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    board_backlight_set(60);
    ESP_LOGI(TAG, "display up (GC9A01, %d MHz), test pattern drawn", LCD_PCLK_HZ / 1000000);
}

void board_display_set_flush_cb(board_flush_done_cb_t cb, void *ctx)
{
    s_flush_ctx = ctx;
    s_flush_cb = cb;
}

void board_display_draw(int x1, int y1, int x2, int y2, const void *data)
{
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, data);
}

static void touch_init(void)
{
    gpio_config_t rst = { .pin_bit_mask = 1ULL << BOARD_PIN_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rst);
    gpio_set_level(BOARD_PIN_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_PIN_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(80));

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1, .sda_io_num = BOARD_PIN_TP_SDA, .scl_io_num = BOARD_PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &s_i2c));
    s_tp_ok = (i2c_master_probe(s_i2c, TP_ADDR, 100) == ESP_OK);
    ESP_LOGI(TAG, "touch CST816D at 0x%02x: %s", TP_ADDR, s_tp_ok ? "ACK" : "no answer");
    if (s_tp_ok) {
        i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000 };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c, &dev, &s_tp));
    }
}

bool board_touch_present(void) { return s_tp_ok; }

bool board_touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_tp_ok) return false;
    uint8_t reg = 0x01, d[6] = { 0 };
    if (i2c_master_transmit_receive(s_tp, &reg, 1, d, sizeof d, 20) != ESP_OK) return false;
    if (d[1] == 0) return false; /* finger count */
    uint16_t rx = ((d[2] & 0x0F) << 8) | d[3];
    uint16_t ry = ((d[4] & 0x0F) << 8) | d[5];
    /* The CST816D reports ghost touches such as x=64 y=3840 (about every 16 s while the backlight is dimmed). They
     * are far outside the 240x240 panel and woke the idle dimmer, so drop anything out of range. */
    if (rx >= BOARD_LCD_W || ry >= BOARD_LCD_H) {
        static uint32_t ghosts;
        if ((ghosts++ % 20) == 0) ESP_LOGW(TAG, "ignoring out-of-range touch x=%u y=%u (%lu so far)", rx, ry, (unsigned long)ghosts);
        return false;
    }
    int dx = (int)rx - BOARD_LCD_W / 2, dy = (int)ry - BOARD_LCD_H / 2;
    if (BOARD_TOUCH_DEADZONE_RADIUS > 0 && dx * dx + dy * dy < BOARD_TOUCH_DEADZONE_RADIUS * BOARD_TOUCH_DEADZONE_RADIUS)
        return false; /* a knob push lands here, see board.h */
    *x = rx;
    *y = ry;
    return true;
}

void board_input_init(void)
{
    pcnt_unit_config_t unit = { .low_limit = -30000, .high_limit = 30000 };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit, &s_pcnt));
    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &filt));

    pcnt_channel_handle_t ca, cb;
    pcnt_chan_config_t a = { .edge_gpio_num = BOARD_PIN_ENC_A, .level_gpio_num = BOARD_PIN_ENC_B };
    pcnt_chan_config_t b = { .edge_gpio_num = BOARD_PIN_ENC_B, .level_gpio_num = BOARD_PIN_ENC_A };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &a, &ca));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &b, &cb));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));

    /* Elecrow reads A/B as plain INPUT, so any pull-up is external. Add internal ones, harmless if so. */
    gpio_set_pull_mode(BOARD_PIN_ENC_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(BOARD_PIN_ENC_B, GPIO_PULLUP_ONLY);
    gpio_config_t sw = { .pin_bit_mask = 1ULL << BOARD_PIN_ENC_SW, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&sw);
    ESP_LOGI(TAG, "encoder (PCNT x4) A=%d B=%d, button=%d", BOARD_PIN_ENC_A, BOARD_PIN_ENC_B, BOARD_PIN_ENC_SW);

    touch_init();
}

int board_encoder_count(void)
{
    int c = 0;
    pcnt_unit_get_count(s_pcnt, &c);
    return c;
}

int board_encoder_take_steps(void)
{
    int c = board_encoder_count();
    if (BOARD_ENC_INVERT) c = -c;
    int steps = (c - s_enc_taken) / BOARD_ENC_EDGES_PER_STEP; /* truncates toward zero, keeps the remainder */
    s_enc_taken += steps * BOARD_ENC_EDGES_PER_STEP;
    if (c > 10000 || c < -10000) {
        /* Stay far from the PCNT limits (+-30000, where it wraps to zero). An edge arriving between the read and the
         * clear is lost, which is acceptable for a spike and happens once per 2500 steps. */
        int rem = c - s_enc_taken;
        pcnt_unit_clear_count(s_pcnt);
        s_enc_taken = -rem; /* so the next call sees the remainder again */
    }
    return steps;
}

bool board_button_pressed(void) { return gpio_get_level(BOARD_PIN_ENC_SW) == 0; }
