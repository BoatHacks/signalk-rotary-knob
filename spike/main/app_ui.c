/* Spike: one test screen for the encoder (rotation + push), touch and the idle dimmer. Throwaway code.
 *
 * What to expect on the device: four coloured markers at the edges (top red, right green, bottom blue, left yellow, the
 * same orientation as the raw test pattern), a ring (arc) with a value in the middle. Turning the knob clockwise should
 * raise the value, a push shows PRESSED and counts clicks, a touch moves the dot to the touch point. After
 * DIM_AFTER_MS without activity the backlight drops to DIM_PERCENT, and any turn or push brings it back. */
#include "app_ui.h"

#include <stdio.h>

#include "board.h"
#include "esp_log.h"
#include "lv_port.h"

static const char *TAG = "app_ui";

#define BRIGHT_PERCENT 60
#define DIM_PERCENT    10    /* never fully off, see SPEC: dim, not dark, while an alarm may be active */
#define DIM_AFTER_MS   10000 /* short on purpose, to test wake from the encoder at the bench */

static lv_obj_t *s_arc;
static lv_obj_t *s_value;
static lv_obj_t *s_status;
static lv_obj_t *s_dot;
static int s_clicks;
static bool s_dimmed;

static lv_obj_t *marker(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, col, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static void on_arc_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        int v = lv_arc_get_value(s_arc);
        lv_label_set_text_fmt(s_value, "%d", v);
        ESP_LOGI(TAG, "encoder value %d", v);
    } else if (code == LV_EVENT_PRESSED) {
        lv_label_set_text(s_status, "PRESSED");
        ESP_LOGI(TAG, "button down");
    } else if (code == LV_EVENT_RELEASED) {
        lv_label_set_text_fmt(s_status, "clicks %d", s_clicks);
        ESP_LOGI(TAG, "button up");
    } else if (code == LV_EVENT_CLICKED) {
        s_clicks++;
        lv_label_set_text_fmt(s_status, "clicks %d", s_clicks);
    }
}

static void on_screen_pressing(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_obj_set_pos(s_dot, p.x - 6, p.y - 6);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

static void on_screen_released(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI(TAG, "touch at x=%d y=%d", (int)p.x, (int)p.y);
    }
}

/* Dimmer: LVGL resets the display inactivity time on encoder rotation, push and touch, so polling it is enough. */
static void dimmer_cb(lv_timer_t *t)
{
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s_dimmed && idle >= DIM_AFTER_MS) {
        s_dimmed = true;
        board_backlight_set(DIM_PERCENT);
        ESP_LOGI(TAG, "idle %u ms: dim to %d%%", (unsigned)idle, DIM_PERCENT);
    } else if (s_dimmed && idle < DIM_AFTER_MS) {
        s_dimmed = false;
        board_backlight_set(BRIGHT_PERCENT);
        ESP_LOGI(TAG, "activity: back to %d%%", BRIGHT_PERCENT);
    }
}

void app_ui_init(void)
{
    lv_port_lock();
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, on_screen_released, LV_EVENT_RELEASED, NULL);

    /* Orientation markers, same colours and sides as the raw test pattern in board_display_init(). */
    marker(scr, 90, 2, 60, 10, lv_color_make(255, 0, 0));
    marker(scr, 228, 90, 10, 60, lv_color_make(0, 255, 0));
    marker(scr, 90, 228, 60, 10, lv_color_make(0, 0, 255));
    marker(scr, 2, 90, 10, 60, lv_color_make(255, 255, 0));

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 190, 190);
    lv_obj_center(s_arc);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 50);
    lv_obj_add_event_cb(s_arc, on_arc_event, LV_EVENT_ALL, NULL);
    lv_group_add_obj(lv_port_group(), s_arc);
    lv_group_set_editing(lv_port_group(), true); /* rotation changes the value straight away, no push needed first */

    s_value = lv_label_create(scr);
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_value, lv_color_white(), 0);
    lv_label_set_text(s_value, "50");
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -8);

    s_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_status, lv_color_make(160, 160, 160), 0);
    lv_label_set_text(s_status, "clicks 0");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 22);

    s_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_make(0, 200, 255), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);

    lv_timer_create(dimmer_cb, 200, NULL);
    lv_port_unlock();
}
