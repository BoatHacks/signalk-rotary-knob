/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_power.h"

#include "knob_logic.h"

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "knob_power";

#define LED_PERIOD_MS 40
#define LED_MAX       64 /* WS2812s at full power are blinding on a dark helm, scale to a quarter */

static volatile knob_led_alarm_t s_alarm;
static volatile uint8_t s_px[BOARD_LED_COUNT][3]; /* the steady frame, per LED (one colour on all five, or a level) */
static volatile bool s_level;                      /* s_px is a level pattern (#9): off at night */
static volatile bool s_night;

/* Steering indicator kind, set by knob_power_apply_snapshot. At night only these are shown (steady colours are off). */
enum { STEER_NONE = 0, STEER_PORT, STEER_STBD, STEER_YELLOW, STEER_BLUE };
static volatile uint8_t s_steer;
static volatile uint32_t s_oncourse_since_ms; /* when the indicator last changed to yellow or blue */

static volatile uint8_t s_full_pct = 100;  /* active brightness from the settings */
static volatile uint8_t s_req_pct = 100;   /* what the UI asked for last */
static volatile bool s_leds_on = true;     /* settings: false keeps every LED dark, the alarm included */


static void apply_backlight(void)
{
    uint8_t p = s_req_pct;
    if (p >= 100 || p > s_full_pct) p = s_full_pct; /* 100 is "active", the dim level never beats it */
    board_backlight_set(p);
}

void knob_power_set_backlight(uint8_t percent)
{
    s_req_pct = percent > 100 ? 100 : percent;
    apply_backlight();
}

void knob_power_set_brightness(uint8_t percent)
{
    s_full_pct = percent < 10 ? 10 : percent > 100 ? 100 : percent;
    apply_backlight();
}

void knob_power_set_leds(bool on) { s_leds_on = on; }

void knob_power_led_alarm(knob_led_alarm_t mode) { s_alarm = mode; }

void knob_power_led_state(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < BOARD_LED_COUNT; i++) { s_px[i][0] = r; s_px[i][1] = g; s_px[i][2] = b; }
    s_level = false;
}

void knob_power_set_night(bool night) { s_night = night; }

static void led_task(void *arg)
{
    for (;;) {
        uint32_t t = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t r = 0, g = 0, b = 0;
        bool per_led = false;                         /* the frame is s_px, not one colour on all five */
        switch (s_alarm) {
        case KNOB_LED_ALARM_FAST:
            r = ((t / 150) & 1) ? LED_MAX : 0;
            break;
        case KNOB_LED_ALARM_SLOW: {
            uint32_t ph = t % 2000; /* triangle wave, 2 s period */
            uint32_t tri = ph < 1000 ? ph : 2000 - ph;
            r = (uint8_t)(LED_MAX * tri / 1000);
            break;
        }
        default:
            if (!s_night) {
                per_led = true;
            } else if (s_level) {
                /* night: the level patterns are off (#9) */
            } else if (s_steer == STEER_PORT) {
                r = LED_MAX;                          /* night: red and green stay on while steering */
            } else if (s_steer == STEER_STBD) {
                g = LED_MAX;
            } else if (s_steer == STEER_YELLOW || s_steer == STEER_BLUE) {
                /* On course (yellow engaged, blue standby): shown for KNOB_LED_NIGHT_FADE_HOLD_MS after the change to
                 * it, then fades linearly to off over KNOB_LED_NIGHT_FADE_MS. */
                uint32_t el = t - s_oncourse_since_ms;
                uint32_t permille = knob_fade_permille(el, KNOB_LED_NIGHT_FADE_HOLD_MS, KNOB_LED_NIGHT_FADE_MS);
                if (s_steer == STEER_YELLOW) { r = (uint8_t)(64 * permille / 1000); g = (uint8_t)(40 * permille / 1000); }
                else b = (uint8_t)(64 * permille / 1000);
            }
            break;
        }
        uint8_t f[BOARD_LED_COUNT][3];
        for (int i = 0; i < BOARD_LED_COUNT; i++) {
            f[i][0] = per_led ? s_px[i][0] : r;
            f[i][1] = per_led ? s_px[i][1] : g;
            f[i][2] = per_led ? s_px[i][2] : b;
            if (!s_leds_on) f[i][0] = f[i][1] = f[i][2] = 0;
        }
        /* Send a frame only when a colour changed: fades and pulses change every period, a steady state (dark included) sends
         * nothing, so a disturbed frame cannot show up again and again. */
        static uint8_t last[BOARD_LED_COUNT][3];
        static bool sent;
        static uint32_t last_sent_ms;
        bool changed = !sent || t - last_sent_ms >= 5000; /* and every 5 s, so a frame that did get disturbed heals */
        for (int i = 0; i < BOARD_LED_COUNT; i++) {
            uint8_t lr = f[i][0], lg = f[i][1], lb = f[i][2];
            if (last[i][0] != lr || last[i][1] != lg || last[i][2] != lb) {
                last[i][0] = lr; last[i][1] = lg; last[i][2] = lb;
                changed = true;
            }
            board_led_set(i, lr, lg, lb);
        }
        if (changed) {
            board_led_show();
            sent = true;
            last_sent_ms = t;
        }
        vTaskDelay(pdMS_TO_TICKS(LED_PERIOD_MS));
    }
}

void knob_power_init(void)
{
    board_led_init();
    xTaskCreateWithCaps(led_task, "leds", 3072, NULL, 3, NULL, MALLOC_CAP_SPIRAM); /* stack in PSRAM: internal RAM is the scarce resource */
    ESP_LOGI(TAG, "LED task up");
}

/* --- Steady colour from the state ------------------------------------------------------------------------------ */
#include <math.h>

/* Level colours (#9), at LED_MAX scale before the brightness. */
static const uint8_t COL_MUSIC[3] = { 24, 8, 64 };  /* violet-blue */
static const uint8_t COL_WARM[3]  = { 64, 40, 16 }; /* warm white */
static const uint8_t COL_PLAY[3]  = { 0, 64, 0 };   /* green */

/* The level fill order around the rim, clockwise from the bottom left: 8:30, 11, 1 and 3:30 (docs/hardware.md). */
static const uint8_t FILL_IDX[4] = { 3, 2, 1, 0 };
#define LED_BOTTOM 4                                 /* 6 o'clock */

/* What the screen's backlight is now, 0..100: the level LEDs follow the brightness setting and the idle dim. */
static uint32_t led_pct(void)
{
    uint32_t p = s_req_pct;
    if (p >= 100 || p > s_full_pct) p = s_full_pct;
    return p;
}

static void px_set(uint8_t f[][3], int i, const uint8_t col[3], uint32_t permille)
{
    uint32_t k = permille * led_pct();                /* 0..100000 */
    for (int c = 0; c < 3; c++) f[i][c] = (uint8_t)((col[c] * k + 50000) / 100000);
}

/* Music and lights (#9): the reported level on four LEDs, the bottom LED for play or on. Stale data: all off. */
static void level_pattern(const marine_ui_snapshot_t *s, marine_ui_mode_t mode)
{
    uint8_t f[BOARD_LED_COUNT][3];
    memset(f, 0, sizeof(f));
    uint16_t lv[4];
    if (mode == MARINE_MODE_DIMMER) {
        uint8_t i = marine_ui_get_selected_light();
        bool fresh = s->dimmer_updated_ms != 0 && !knob_stale_by(s->now_ms, s->dimmer_updated_ms, s->dimmer_timeout_ms);
        if (fresh && i < s->dimmer_group_count && s->dimmer_groups[i].on) {
            const marine_dimmer_group_t *g = &s->dimmer_groups[i];
            if (g->onoff_only) {
                for (int k = 0; k < BOARD_LED_COUNT; k++) px_set(f, k, COL_WARM, 1000);
            } else {
                knob_led_fill(g->level_pct / 100.0f, 4, lv);
                for (int k = 0; k < 4; k++) px_set(f, FILL_IDX[k], COL_WARM, lv[k]);
                px_set(f, LED_BOTTOM, COL_WARM, 1000);
            }
        }
    } else {
        uint8_t i = marine_ui_get_selected_vol_target();
        if (i < s->vol_target_count) {
            const marine_vol_target_t *v = &s->vol_targets[i];
            bool fresh = v->connected && v->updated_ms != 0 && !knob_stale_by(s->now_ms, v->updated_ms, v->timeout_ms);
            if (fresh) {
                knob_led_fill(v->pct / 100.0f, 4, lv);
                for (int k = 0; k < 4; k++) px_set(f, FILL_IDX[k], COL_MUSIC, v->muted ? lv[k] / 4u : lv[k]);
                if (s->play_state == MARINE_PLAY_PLAYING) px_set(f, LED_BOTTOM, COL_PLAY, 1000);
            }
        }
    }
    for (int i = 0; i < BOARD_LED_COUNT; i++) { s_px[i][0] = f[i][0]; s_px[i][1] = f[i][1]; s_px[i][2] = f[i][2]; }
    s_level = true;
}

void knob_power_apply_snapshot(const marine_ui_snapshot_t *s, marine_ui_mode_t active_mode)
{
    static bool on_course;
    uint8_t r = 0, g = 0, b = 0;
    uint8_t steer = STEER_NONE;

    if (active_mode == MARINE_MODE_AUTOPILOT) {
        /* Steering indicator on the autopilot screen, engaged or in standby. Standby uses the set target. */
        /* The error follows the pilot's state (#7): compass the target minus the heading, wind the apparent wind angle minus
         * its target (a bigger starboard angle needs a turn to port), route the side to steer from the cross-track error. */
        bool wind = s->ap_engaged && s->ap_state == KNOB_AP_WIND;
        bool route = s->ap_engaged && s->ap_state == KNOB_AP_ROUTE;
        bool stale;
        if (wind) stale = knob_stale_by(s->now_ms, s->wind_updated_ms, s->wind_timeout_ms) || !s->wind_target_valid;
        else if (route) stale = s->route_updated_ms == 0 || knob_stale_by(s->now_ms, s->route_updated_ms, s->route_timeout_ms);
        else stale = knob_stale_by(s->now_ms, s->heading_updated_ms, s->heading_timeout_ms) ||
                     knob_stale_by(s->now_ms, s->cts_updated_ms, s->cts_timeout_ms);
        if (stale) {
            on_course = false;
            if (s->ap_engaged) { r = 40; g = 24; b = 0; } /* dim amber: no usable course error */
        } else {
            float err;                                                 /* negative: turn to port */
            if (wind) err = knob_course_error(s->wind_awa_deg, s->wind_target_deg);
            else if (route) err = (float)knob_xte_steer(s->route_xte_m, 10.0f) * 5.0f; /* 0 on track, else past the leave threshold */
            else err = knob_course_error(s->cts_deg, s->heading_deg);
            knob_steer_t k = knob_steer_step(err, &on_course, KNOB_LED_ON_COURSE_ENTER_DEG, KNOB_LED_ON_COURSE_LEAVE_DEG);
            if (k == KNOB_STEER_ON_COURSE && s->ap_engaged) { r = 64; g = 40; b = 0; steer = STEER_YELLOW; }
            else if (k == KNOB_STEER_ON_COURSE) { b = 64; steer = STEER_BLUE; }
            else if (k == KNOB_STEER_PORT) { r = 64; steer = STEER_PORT; }
            else { g = 64; steer = STEER_STBD; }
        }
    } else {
        on_course = false;
        if ((active_mode == MARINE_MODE_DIMMER || active_mode == MARINE_MODE_MUSIC) && s->active_alarm == MARINE_ALARM_NONE) {
            s_steer = STEER_NONE;
            level_pattern(s, active_mode);
            return;
        }
        if (s->ap_engaged) g = 40;
        else if (s->anchor_dropped) { g = 20; b = 40; }
    }
    bool oncourse_kind = steer == STEER_YELLOW || steer == STEER_BLUE;
    if (oncourse_kind && s_steer != steer) s_oncourse_since_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_steer = steer;
    knob_power_led_state(r, g, b);
}
