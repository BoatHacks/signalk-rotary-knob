/* SPDX-License-Identifier: Apache-2.0
 * Encoder timing spike (#8): how many detents one full turn has, how many quadrature edges one detent is, and how far apart
 * the clicks are when the knob is turned slowly, normally and fast. The motor mode's speed-dependent steps take their
 * thresholds from this.
 *
 * Same PCNT setup as the knob firmware's board.c (x4 quadrature, 1 us glitch filter). The count is sampled every 1 ms.
 * Press the knob to start a run, turn, press again to end it and print the summary. Nothing else runs (no display, no
 * Wi-Fi, no espOS). */
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_RAIL_1      1   /* same as the knob's board.h */
#define PIN_RAIL_2      2
#define PIN_ENC_A       45
#define PIN_ENC_B       42
#define PIN_ENC_SW      41
#define EDGES_PER_STEP  4   /* the knob firmware's assumption, checked by the rest positions below */
#define REST_MS         300 /* still this long = the knob is resting in a detent */
#define PAUSE_MS        1000 /* a gap longer than this is a pause, not part of a turn */
#define MAX_CLICKS      2000

static const char *TAG = "enc";
static pcnt_unit_handle_t s_pcnt;
static uint32_t s_iv[MAX_CLICKS];   /* ms between consecutive detents within a run */

static int count_now(void)
{
    int c = 0;
    pcnt_unit_get_count(s_pcnt, &c);
    return c;
}

static void encoder_init(void)
{
    pcnt_unit_config_t unit = { .low_limit = -30000, .high_limit = 30000 };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit, &s_pcnt));
    pcnt_glitch_filter_config_t filt = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt, &filt));
    pcnt_channel_handle_t ca, cb;
    pcnt_chan_config_t a = { .edge_gpio_num = PIN_ENC_A, .level_gpio_num = PIN_ENC_B };
    pcnt_chan_config_t b = { .edge_gpio_num = PIN_ENC_B, .level_gpio_num = PIN_ENC_A };
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &a, &ca));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt, &b, &cb));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(ca, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(ca, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(cb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(cb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));
    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt));
    gpio_set_pull_mode(PIN_ENC_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_ENC_B, GPIO_PULLUP_ONLY);
    gpio_config_t sw = { .pin_bit_mask = 1ULL << PIN_ENC_SW, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&sw));
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t pct(const uint32_t *v, int n, int p)
{
    int i = (n - 1) * p / 100;
    return v[i];
}

static int s_res[EDGES_PER_STEP];   /* rest positions by count modulo EDGES_PER_STEP */

static void summary(int run, int edges, int detents, int n_iv, int rests)
{
    ESP_LOGI(TAG, "==== run %d done ====", run);
    ESP_LOGI(TAG, "edges %d (net, signed), detents %d at %d edges each", edges, detents, EDGES_PER_STEP);
    /* All rests in one residue = one detent is EDGES_PER_STEP edges (the residue is only where the count started). Rests
     * spread over two residues 2 apart = a detent is half that. */
    ESP_LOGI(TAG, "rests %d, by count mod %d: %d %d %d %d", rests, EDGES_PER_STEP, s_res[0], s_res[1], s_res[2], s_res[3]);
    if (n_iv == 0) { ESP_LOGI(TAG, "no clicks within a turn (pauses over %d ms are left out)", PAUSE_MS); return; }
    qsort(s_iv, n_iv, sizeof(s_iv[0]), cmp_u32);
    ESP_LOGI(TAG, "ms between clicks (%d): min %" PRIu32 " p10 %" PRIu32 " median %" PRIu32 " p90 %" PRIu32 " max %" PRIu32,
             n_iv, s_iv[0], pct(s_iv, n_iv, 10), pct(s_iv, n_iv, 50), pct(s_iv, n_iv, 90), s_iv[n_iv - 1]);
    static const uint32_t edge[] = { 20, 30, 50, 80, 150, 300, PAUSE_MS };
    int lo = 0;
    for (size_t b = 0; b < sizeof(edge) / sizeof(edge[0]); b++) {
        int k = 0;
        for (int i = 0; i < n_iv; i++) if (s_iv[i] >= (uint32_t)lo && s_iv[i] < edge[b]) k++;
        ESP_LOGI(TAG, "  %4d..%4" PRIu32 " ms: %d", lo, edge[b], k);
        lo = (int)edge[b];
    }
}

void app_main(void)
{
    gpio_config_t out = { .pin_bit_mask = (1ULL << PIN_RAIL_1) | (1ULL << PIN_RAIL_2), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_RAIL_1, 1);
    gpio_set_level(PIN_RAIL_2, 1);
    encoder_init();
    ESP_LOGI(TAG, "encoder timing spike: press to start a run, turn, press again for the summary");

    bool running = false, btn_was = false;
    int run = 0, start = 0, last = count_now(), last_step = 0, n_iv = 0, rests = 0, last_rest = last;
    int64_t last_click_us = 0, last_move_us = 0, btn_us = 0;
    bool resting = true;

    for (;;) {
        int64_t now = esp_timer_get_time();
        bool btn = gpio_get_level(PIN_ENC_SW) == 0;
        if (btn && !btn_was && now - btn_us > 200000) {   /* press, 200 ms debounce */
            btn_us = now;
            if (!running) {
                running = true;
                run++;
                start = count_now();
                last_step = start / EDGES_PER_STEP;
                n_iv = rests = 0;
                memset(s_res, 0, sizeof(s_res));
                last_rest = start;
                last_click_us = 0;
                ESP_LOGI(TAG, "==== run %d started at count %d, turn now ====", run, start);
            } else {
                running = false;
                int edges = count_now() - start;
                summary(run, edges, abs(edges) / EDGES_PER_STEP, n_iv, rests);
            }
        }
        btn_was = btn;

        int c = count_now();
        if (c != last) {
            last = c;
            last_move_us = now;
            resting = false;
            /* a detent passed when the count crosses a multiple of EDGES_PER_STEP */
            int step = c >= 0 ? c / EDGES_PER_STEP : -((-c + EDGES_PER_STEP - 1) / EDGES_PER_STEP);
            if (running && step != last_step) {
                int d = abs(step - last_step);
                for (int i = 0; i < d; i++) {
                    if (last_click_us) {
                        uint32_t ms = (uint32_t)((now - last_click_us) / 1000);
                        if (ms < PAUSE_MS && n_iv < MAX_CLICKS) s_iv[n_iv++] = i == 0 ? ms : 0;
                    }
                    last_click_us = now;
                }
                ESP_LOGI(TAG, "click %+d  count %d  %" PRId64 " ms", step > last_step ? 1 : -1, c, now / 1000);
            }
            last_step = step;
        } else if (!resting && now - last_move_us > REST_MS * 1000) {
            resting = true;
            if (running) {
                rests++;
                s_res[((c % EDGES_PER_STEP) + EDGES_PER_STEP) % EDGES_PER_STEP]++;
                ESP_LOGI(TAG, "rest at count %d (%+d edges since the last rest)", c, c - last_rest);
                last_rest = c;
            }
        }
        vTaskDelay(1);
    }
}
