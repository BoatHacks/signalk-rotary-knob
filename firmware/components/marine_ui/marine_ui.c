#include "marine_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Configurable Timing & Power Constants ---
// Per-value stale timeouts are NOT constants here: the firmware passes `<name>_timeout_ms` in the snapshot
// (from SignalK meta.timeout or a per-path default), 0 = never stale by age.
#define MARINE_TICK_MIN_DELTA_DEG       0.5f     // compass tick ring repaints only after >= 0.5 deg of heading change
#define MARINE_TICK_MIN_INTERVAL_MS     100U     // ... and at most 10 times a second
#define MARINE_IDLE_DIM_TIME_MS         30000U   // 30 s idle -> dim backlight
#define MARINE_IDLE_DIM_LEVEL_PCT       10U      // 10 % backlight when dimmed
#define MARINE_IDLE_OFF_TIME_MS         600000U  // 10 min idle -> 0 % backlight (unless AP engaged or Alarm active)
#define MARINE_FACTORY_RESET_HOLD_MS    5000U    // 5 s continuous hold to trigger factory reset
#define MARINE_PENDING_CONFIRM_TIMEOUT  3000U    // 3 s max hold on pending value awaiting server confirm

// --- Day & Night Color Tokens (Day muted = #8CA0BC) ---
#define COL_BG          lv_color_hex(0x030811)
#define COL_CARD        lv_color_hex(0x0B1628)
#define COL_CYAN        lv_color_hex(0x00F0FF)
#define COL_AMBER       lv_color_hex(0xFFAA00)
#define COL_GREEN       lv_color_hex(0x00E676)
#define COL_RED         lv_color_hex(0xFF2A3C)
#define COL_PURPLE      lv_color_hex(0xB388FF)
#define COL_GOLD        lv_color_hex(0xFFD54F)
#define COL_TEXT_MAIN   lv_color_hex(0xF0F6FC)
#define COL_TEXT_MUTED  lv_color_hex(0x8CA0BC)
#define COL_NIGHT_RED   lv_color_hex(0xFF1E27)
#define COL_NIGHT_DIM   lv_color_hex(0x7A1014)

typedef struct {
    marine_ui_mode_t id;
    const char *ring_label; // AUTO, LIGHT, ANCHOR, USB
    lv_obj_t *(*build_screen)(void);
    void (*on_input)(marine_semantic_input_t ev, int32_t val);
    void (*refresh)(void);
} marine_mode_desc_t;

// Fixed Mode Ring slots: 0=AUTO (Top), 1=LIGHT (Right), 2=ANCHOR (Bottom), 3=USB (Left)
static const marine_ui_mode_t ring_slot_modes[MARINE_MODE_COUNT] = {
    MARINE_MODE_AUTOPILOT,
    MARINE_MODE_DIMMER,
    MARINE_MODE_ANCHOR_WATCH,
    MARINE_MODE_USB_HID
};

static const char *rgbw_preset_names[MARINE_RGBW_PRESET_COUNT] = {
    "NIGHT RED", "WARM WHITE", "COOL WHITE", "AMBER DECK", "OCEAN CYAN", "DEEP BLUE", "EMERALD"
};
static const uint32_t rgbw_preset_hex[MARINE_RGBW_PRESET_COUNT] = {
    0xFF1E27, 0xFFCF87, 0xE8F4FF, 0xFF9800, 0x00E5FF, 0x2979FF, 0x00E676
};

// --- Internal UI State (Zero authoritative server state ownership) ---
static marine_ui_snapshot_t snap;
static marine_ui_action_cb_t s_action_cb = NULL;
static void *s_action_ctx = NULL;
static marine_ui_mode_t active_mode = MARINE_MODE_AUTOPILOT;
static marine_alarm_type_t prev_alarm_state = MARINE_ALARM_NONE;
static bool prev_ack_state = false;

// Stale tracking (evaluated every tick, not only when a snapshot arrives)
typedef enum { STALE_NONE = 0, STALE_LINK, STALE_ESSENTIAL } stale_state_t;
static uint32_t last_stale_sig = 0xFFFFFFFFu;
static const char *last_stale_reason = NULL;
static uint32_t last_stale_age_s = 0xFFFFFFFFu;

// Semantic Button Classifier State
static bool btn_is_down = false;
static uint32_t btn_down_ms = 0;
static uint8_t btn_click_count = 0;
static uint32_t btn_last_up_ms = 0;
static bool btn_long_fired = false;

// UI Transient State
static bool mode_ring_open = false;
static uint8_t ring_selected_slot = 0;
static uint32_t ring_opened_ms = 0;

static uint8_t ap_step_deg = 1;
static bool ap_pending_active = false;
static bool ap_pending_sent = false;
static int32_t ap_pending_cts = 0;
static uint32_t ap_pending_deadline_ms = 0;
static bool ap_confirm_open = false;
static uint32_t ap_confirm_deadline_ms = 0;

static bool anchor_preview_active = false;
static uint16_t anchor_preview_radius = 35;
static bool anchor_menu_open = false;
static uint8_t anchor_menu_idx = 0;
static uint32_t anchor_menu_opened_ms = 0;

static uint8_t dimmer_selected_group = 0;
static bool dimmer_pending_active = false;
static bool dimmer_pending_sent = false;
static uint8_t dimmer_pending_level = 0;
static uint8_t dimmer_pending_preset = 0;
static uint32_t dimmer_pending_deadline_ms = 0;
// A switched-off group that remembers its level: the first turn click only switches it back on (swallowed, no level
// change). The latch stays set until the server shows the group on, or the confirm timeout passes, so a quick second click
// cannot send a second toggle or change the level before the switch-on has landed.
static bool dimmer_wake_active = false;
static uint8_t dimmer_wake_group = 0;
static uint32_t dimmer_wake_deadline_ms = 0;

static uint32_t last_input_ms = 0;
static uint8_t current_backlight_pct = 100;
static bool reset_holding = false;
static uint32_t reset_hold_start_ms = 0;

// LVGL Screens & Overlays
static lv_obj_t *scr_modes[MARINE_MODE_COUNT];
static lv_obj_t *scr_system;
static lv_obj_t *ovl_mode_ring;
static lv_obj_t *ovl_alarm;
static lv_obj_t *ovl_alarm_ack_ring;
static lv_obj_t *ovl_alarm_ack_badge;
static lv_obj_t *ovl_alarm_ack_lbl;
static lv_obj_t *ovl_stale;
static lv_obj_t *stale_lbl_title;
static lv_obj_t *stale_lbl_sub;
static lv_obj_t *ovl_ap_confirm;
static lv_obj_t *ovl_anc_menu;

// Autopilot Widgets (Radial rotating tick lines)
static lv_obj_t *ap_tick_lines[72];
static lv_point_precise_t ap_tick_pts[72][2];
static lv_obj_t *ap_cts_bug;
static lv_obj_t *ap_lbl_hdg;
static lv_obj_t *ap_pill_mode;
static lv_obj_t *ap_lbl_mode;
static lv_obj_t *ap_lbl_cts;
static lv_obj_t *ap_lbl_delta;
static lv_obj_t *ap_rudder_fill;
static lv_obj_t *ap_lbl_telem;

// Anchor Widgets & Cached Rotated-Glyph Curved Text Canvases (PSRAM-backed)
static lv_obj_t *anc_ring;
static lv_obj_t *anc_boat_dot;
static lv_obj_t *anc_lbl_center;
static lv_obj_t *anc_canvas_top;
static lv_obj_t *anc_canvas_bot;
static uint8_t *anc_buf_top = NULL;
static uint8_t *anc_buf_bot = NULL;
static char anc_cache_top[48] = {0};
static char anc_cache_bot[48] = {0};

// Dimmer Widgets
static lv_obj_t *dim_arc;
static lv_obj_t *dim_rgbw_swatch;
static lv_obj_t *dim_lbl_grp_count;
static lv_obj_t *dim_dots[MARINE_MAX_DIMMER_GROUPS];
static lv_obj_t *dim_lbl_val;
static lv_obj_t *dim_lbl_name;
static lv_obj_t *dim_lbl_hint;

// USB-HID Widgets (No Gauge Arc)
static lv_obj_t *hid_lbl_status;
static lv_obj_t *hid_lbl_profile;
static lv_obj_t *hid_lbl_sub;
static lv_obj_t *hid_lbl_hint;

// Mode Ring Widgets (including Live Shrunk Center Preview)
static lv_obj_t *ring_slot_boxes[4];
static lv_obj_t *ring_slot_labels[4];
static lv_obj_t *ring_slot_dots[4];
static lv_obj_t *ring_slot_icons[4];
static uint8_t *ring_icon_buf[4];
static uint32_t ring_icon_col[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
static lv_obj_t *ring_center_top;
static lv_obj_t *ring_center_mid;
static lv_obj_t *ring_center_bot;

// Alarm Widgets + Cached Curved Text Canvases
static lv_obj_t *alm_geofence;
static lv_obj_t *alm_boat_dot;
static lv_obj_t *alm_lbl_title;
static lv_obj_t *alm_lbl_ack;
static lv_obj_t *alm_icon_anchor;
static lv_obj_t *alm_icon_warn;
static uint8_t *alm_icon_anchor_buf = NULL;
static uint8_t *alm_icon_warn_buf = NULL;
static uint32_t alm_icon_col = 0xFFFFFFFFu;
static lv_obj_t *alm_canvas_top;
static lv_obj_t *alm_canvas_bot;
static uint8_t *alm_buf_top = NULL;
static uint8_t *alm_buf_bot = NULL;
static char alm_cache_top[48] = {0};
static char alm_cache_bot[48] = {0};

// Stale, Confirm, Anchor Menu & System Screen Widgets
static lv_obj_t *stale_lbl_age;
static lv_obj_t *ap_confirm_arc;
static lv_obj_t *ap_confirm_lbl_sec;
static lv_obj_t *anc_menu_items[2];

static lv_obj_t *sys_tick_lines[72];
static lv_point_precise_t sys_tick_pts[72][2];
static lv_obj_t *sys_canvas_top;
static lv_obj_t *sys_canvas_bot;
static uint8_t *sys_buf_top = NULL;
static uint8_t *sys_buf_bot = NULL;
static char sys_cache_top[48] = {0};
static char sys_cache_bot[48] = {0};
static lv_obj_t *sys_pill_box;
static lv_obj_t *sys_step_dot[3];      // connecting steps: drawn status marks (no font glyphs needed)
static lv_obj_t *sys_step_chk[3];      // LV_SYMBOL_OK inside a finished step's dot
static lv_obj_t *sys_step_lbl[3];
static lv_obj_t *sys_lbl_title;
static lv_obj_t *sys_lbl_line1;
static lv_obj_t *sys_lbl_line2;
static lv_obj_t *sys_arc_prog;

// Boot splash: a full-screen RGB565 picture (splash_img.c, from firmware/assets/splash.png) covers the system screen while
// booting. The picture has its own progress bar baked in. The live bar below is drawn over that bar's inside (x 62..176,
// y 206..214 in the 240 x 240 picture) so the progress shown is real.
extern const lv_image_dsc_t marine_splash_img;
// The picture is baked 1 px right and down with the area outside the glass blacked out (tools/png2lvgl.py). Its circle spans x 0..237 and y 0..239 inside the 240 x 240 PNG, one pixel up
// and left of the glass centre. Everything on the splash shares this offset, so the live bar stays aligned.
#define SPLASH_OFF_X 1
#define SPLASH_OFF_Y 1
#define SPLASH_BAR_X (62 + SPLASH_OFF_X)
#define SPLASH_BAR_Y (206 + SPLASH_OFF_Y)
#define SPLASH_BAR_W 115
#define SPLASH_BAR_H 9
static lv_obj_t *sys_splash;
static lv_obj_t *sys_splash_track;
static lv_obj_t *sys_splash_fill;
// Status bar under the progress bar, in the place of the four baked-in dots (they are erased in splash_img.c). It names
// the current boot phase in text, from snap.boot_phase.
#define SPLASH_PILL_W 84
#define SPLASH_PILL_H 14
#define SPLASH_PILL_Y (218 + SPLASH_OFF_Y)
static lv_obj_t *sys_splash_pill;
static lv_obj_t *sys_splash_phase;

// Shared Scratch Glyph Canvas for Tangent Rotation (24x24 ARGB8888 in PSRAM)
#define GLYPH_TMP_SIZE 24
#define GLYPH_TMP_BYTES (GLYPH_TMP_SIZE * GLYPH_TMP_SIZE * 4)
static lv_obj_t *s_glyph_tmp_cv = NULL;
static uint8_t *s_glyph_tmp_buf = NULL;
static lv_draw_buf_t s_glyph_draw_buf;

// --- Forward Declarations ---
static lv_obj_t* build_ap_screen(void);
static void ap_on_input(marine_semantic_input_t ev, int32_t val);
static void ap_refresh(void);

static lv_obj_t* build_dimmer_screen(void);
static void dimmer_on_input(marine_semantic_input_t ev, int32_t val);
static void dimmer_refresh(void);

static lv_obj_t* build_anchor_screen(void);
static void anchor_on_input(marine_semantic_input_t ev, int32_t val);
static void anchor_refresh(void);

static lv_obj_t* build_hid_screen(void);
static void hid_on_input(marine_semantic_input_t ev, int32_t val);
static void hid_refresh(void);

static const marine_mode_desc_t mode_registry[MARINE_MODE_COUNT] = {
    [MARINE_MODE_AUTOPILOT]    = { MARINE_MODE_AUTOPILOT,    "AUTO",   build_ap_screen,     ap_on_input,     ap_refresh },
    [MARINE_MODE_DIMMER]       = { MARINE_MODE_DIMMER,       "LIGHT",  build_dimmer_screen, dimmer_on_input, dimmer_refresh },
    [MARINE_MODE_ANCHOR_WATCH] = { MARINE_MODE_ANCHOR_WATCH, "ANCHOR", build_anchor_screen, anchor_on_input, anchor_refresh },
    [MARINE_MODE_USB_HID]      = { MARINE_MODE_USB_HID,      "USB",    build_hid_screen,    hid_on_input,    hid_refresh }
};

static void emit_action(marine_ui_mode_t mode, marine_ui_action_type_t act, int32_t val, int32_t meta) {
    if (s_action_cb) {
        marine_ui_action_t payload = { .mode = mode, .action = act, .value = val, .meta = meta };
        s_action_cb(&payload, s_action_ctx);
    }
}

static float wrap_deg(float d) {
    while (d < 0.0f) d += 360.0f;
    while (d >= 360.0f) d -= 360.0f;
    return d;
}

static lv_color_t c_primary(void) { return snap.night_palette ? COL_NIGHT_RED : COL_CYAN; }
static lv_color_t c_text(void)    { return snap.night_palette ? COL_NIGHT_RED : COL_TEXT_MAIN; }
static lv_color_t c_muted(void)   { return snap.night_palette ? COL_NIGHT_DIM : COL_TEXT_MUTED; }
static lv_color_t c_amber(void)   { return snap.night_palette ? COL_NIGHT_RED : COL_AMBER; }
static lv_color_t c_red(void)     { return snap.night_palette ? COL_NIGHT_RED : COL_RED; }

// --- Per-Value Staleness (age vs the value's own timeout, 0 = never stale by age) ---
static bool value_is_stale(uint32_t updated_ms, uint32_t timeout_ms) {
    if (timeout_ms == 0) return false;
    if (updated_ms == 0) return true; // never received
    uint32_t age = (snap.now_ms >= updated_ms) ? (snap.now_ms - updated_ms) : 0;
    return age > timeout_ms;
}

#define SB_RUDDER   (1u << 0)
#define SB_DEPTH    (1u << 1)
#define SB_VOLTAGE  (1u << 2)
#define SB_GPS      (1u << 3)

// Display-only values: a stale one is masked on its own, nothing else changes.
static uint32_t display_stale_bits(void) {
    uint32_t b = 0;
    if (value_is_stale(snap.rudder_updated_ms, snap.rudder_timeout_ms)) b |= SB_RUDDER;
    if (value_is_stale(snap.depth_updated_ms, snap.depth_timeout_ms)) b |= SB_DEPTH;
    if (value_is_stale(snap.voltage_updated_ms, snap.voltage_timeout_ms)) b |= SB_VOLTAGE;
    if (value_is_stale(snap.gps_updated_ms, snap.gps_timeout_ms)) b |= SB_GPS;
    return b;
}

// Essential values of the active mode: returns the reason text for the first stale one, or NULL.
static const char *essential_stale_reason(void) {
    switch (active_mode) {
    case MARINE_MODE_AUTOPILOT:
        if (value_is_stale(snap.ap_state_updated_ms, snap.ap_state_timeout_ms)) return "NO AP STATE";
        if (value_is_stale(snap.heading_updated_ms, snap.heading_timeout_ms)) return "NO HEADING";
        if (value_is_stale(snap.cts_updated_ms, snap.cts_timeout_ms)) return "NO TARGET";
        return NULL;
    case MARINE_MODE_ANCHOR_WATCH:
        if (value_is_stale(snap.anchor_updated_ms, snap.anchor_timeout_ms)) return "NO ANCHOR DATA";
        return NULL;
    case MARINE_MODE_DIMMER:
        if (value_is_stale(snap.dimmer_updated_ms, snap.dimmer_timeout_ms)) return "NO LIGHT DATA";
        return NULL;
    default:
        return NULL; // USB-HID needs the USB host only
    }
}

// HID never depends on the SignalK link. Link down beats essential-value staleness.
static stale_state_t current_stale(void) {
    if (active_mode == MARINE_MODE_USB_HID) return STALE_NONE;
    if (!snap.signalk_connected) return STALE_LINK;
    if (essential_stale_reason() != NULL) return STALE_ESSENTIAL;
    return STALE_NONE;
}

static void update_stale_overlay(void);
static void cancel_transients(void);

static bool is_full_alarm_blocking(void) {
    if (snap.active_alarm == MARINE_ALARM_NONE) return false;
    if (snap.alarm_acknowledged && snap.alarm_ack_return_with_badge) return false;
    return true;
}

// --- Radial Tick Ring Builder & Updater (72 radial lv_line ticks rotating with heading) ---
static void build_radial_tick_ring(lv_obj_t *parent, lv_obj_t *lines[72], lv_point_precise_t pts[72][2]) {
    for (int i = 0; i < 72; i++) {
        lines[i] = lv_line_create(parent);
        bool maj = (i % 6 == 0);
        lv_obj_set_style_line_width(lines[i], maj ? 2 : 1, 0);
        lv_obj_set_style_line_color(lines[i], maj ? COL_TEXT_MAIN : COL_TEXT_MUTED, 0);
        lv_obj_remove_flag(lines[i], LV_OBJ_FLAG_CLICKABLE);
        pts[i][0].x = 120; pts[i][0].y = 4;
        pts[i][1].x = 120; pts[i][1].y = maj ? 15 : 10;
        lv_line_set_points(lines[i], pts[i], 2);
    }
}

// `pts` must stay alive (static arrays): LVGL keeps the pointer. Colours are set only when `recolor` is true.
static void update_radial_tick_ring(lv_obj_t *lines[72], lv_point_precise_t pts[72][2], float heading_deg, bool dimmed_night, bool recolor) {
    for (int i = 0; i < 72; i++) {
        bool maj = (i % 6 == 0);
        float deg = (float)i * 5.0f - heading_deg - 90.0f;
        float rad = deg * (float)(M_PI / 180.0);
        float cos_a = cosf(rad);
        float sin_a = sinf(rad);
        float r_in = maj ? 105.0f : 111.0f;
        float r_out = 116.0f;

        pts[i][0].x = 120.0f + cos_a * r_in;
        pts[i][0].y = 120.0f + sin_a * r_in;
        pts[i][1].x = 120.0f + cos_a * r_out;
        pts[i][1].y = 120.0f + sin_a * r_out;
        lv_line_set_points(lines[i], pts[i], 2);

        if (recolor) {
            lv_color_t tc = dimmed_night
                ? (maj ? lv_color_hex(0x6A1216) : lv_color_hex(0x3A080A))
                : (maj ? COL_TEXT_MAIN : COL_TEXT_MUTED);
            lv_obj_set_style_line_color(lines[i], tc, 0);
        }
    }
}

static float heading_delta_abs(float a, float b) {
    float d = fabsf(a - b);
    while (d >= 360.0f) d -= 360.0f;
    return d > 180.0f ? 360.0f - d : d;
}

// Rate-limited update of the autopilot ring: >= 0.5 deg change and <= 10 Hz. Palette change forces a repaint.
static void ap_tick_ring_maybe_update(void) {
    static float last_heading = -1000.0f;
    static uint32_t last_ms = 0;
    static int8_t last_night = -1;
    bool night = snap.night_palette;
    bool palette_changed = (last_night != (int8_t)night);
    bool moved = (last_heading < -999.0f) || (heading_delta_abs(snap.heading_deg, last_heading) >= MARINE_TICK_MIN_DELTA_DEG);
    bool due = (snap.now_ms - last_ms) >= MARINE_TICK_MIN_INTERVAL_MS;
    if (!palette_changed && !(moved && due)) return;
    update_radial_tick_ring(ap_tick_lines, ap_tick_pts, snap.heading_deg, night, palette_changed);
    last_heading = snap.heading_deg;
    last_ms = snap.now_ms;
    last_night = (int8_t)night;
}

// --- Curved Text Helper with Tangent Glyph Rotation (PSRAM-backed, cached on string change) ---
#define CURVED_CANVAS_W 240
#define CURVED_CANVAS_H 56
#define CURVED_BUF_BYTES (CURVED_CANVAS_W * CURVED_CANVAS_H * 4)

static void ensure_glyph_scratch_canvas(void) {
    if (s_glyph_tmp_cv) return;
    s_glyph_tmp_buf = (uint8_t *)heap_caps_malloc(GLYPH_TMP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_glyph_tmp_buf) s_glyph_tmp_buf = (uint8_t *)lv_malloc(GLYPH_TMP_BYTES);

    lv_draw_buf_init(&s_glyph_draw_buf, GLYPH_TMP_SIZE, GLYPH_TMP_SIZE, LV_COLOR_FORMAT_ARGB8888,
                     GLYPH_TMP_SIZE * 4, s_glyph_tmp_buf, GLYPH_TMP_BYTES);

    s_glyph_tmp_cv = lv_canvas_create(lv_layer_sys());
    lv_canvas_set_buffer(s_glyph_tmp_cv, s_glyph_tmp_buf, GLYPH_TMP_SIZE, GLYPH_TMP_SIZE, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_flag(s_glyph_tmp_cv, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* create_curved_canvas(lv_obj_t *parent, lv_align_t align, uint8_t **out_buf) {
    ensure_glyph_scratch_canvas();
    *out_buf = (uint8_t *)heap_caps_malloc(CURVED_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_buf) *out_buf = (uint8_t *)lv_malloc(CURVED_BUF_BYTES);

    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, *out_buf, CURVED_CANVAS_W, CURVED_CANVAS_H, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_align(cv, align, 0, 0);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

static void update_curved_canvas(lv_obj_t *cv, char *cache, const char *text, bool bottom, lv_color_t col) {
    if (strcmp(cache, text) == 0) return;
    strncpy(cache, text, 47);
    cache[47] = '\0';

    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    size_t len = strlen(text);
    if (len == 0) return;

    const float radius = 98.0f;
    const float step_rad = 0.082f;
    const float total_rad = (float)(len - 1) * step_rad;
    const float center_rad = bottom ? (float)(M_PI / 2.0) : (float)(-M_PI / 2.0);
    const float cy_screen = bottom ? (120.0f - (240.0f - CURVED_CANVAS_H)) : 120.0f;

    lv_draw_label_dsc_t lbl_dsc;
    lv_draw_label_dsc_init(&lbl_dsc);
    lbl_dsc.color = col;
    lbl_dsc.font = &lv_font_montserrat_12;
    lbl_dsc.text_local = true; // Safe per-task text copy in LVGL 9.3 draw queue

    lv_draw_image_dsc_t img_dsc;
    lv_draw_image_dsc_init(&img_dsc);
    img_dsc.src = &s_glyph_draw_buf;
    img_dsc.pivot.x = GLYPH_TMP_SIZE / 2;
    img_dsc.pivot.y = GLYPH_TMP_SIZE / 2;

    char glyph[2] = {0, 0};
    for (size_t i = 0; i < len; i++) {
        glyph[0] = text[i];
        lbl_dsc.text = glyph;

        // 1. Render single glyph into scratch ARGB8888 buffer synchronously
        lv_canvas_fill_bg(s_glyph_tmp_cv, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t g_layer;
        lv_canvas_init_layer(s_glyph_tmp_cv, &g_layer);
        lv_area_t g_area = { 6, 4, GLYPH_TMP_SIZE - 2, GLYPH_TMP_SIZE - 2 };
        lv_draw_label(&g_layer, &lbl_dsc, &g_area);
        lv_canvas_finish_layer(s_glyph_tmp_cv, &g_layer);

        // 2. Compute polar position and tangent angle (0.1 degree units for LVGL 9.3)
        float a = bottom
            ? (center_rad + total_rad * 0.5f - (float)i * step_rad)
            : (center_rad - total_rad * 0.5f + (float)i * step_rad);
        float rot_rad = bottom ? (a - (float)(M_PI / 2.0)) : (a + (float)(M_PI / 2.0));
        img_dsc.rotation = (int32_t)roundf(rot_rad * (1800.0f / (float)M_PI));

        int32_t cx = (int32_t)roundf(120.0f + cosf(a) * radius);
        int32_t cy = (int32_t)roundf(cy_screen + sinf(a) * radius);
        lv_area_t dst_area = {
            cx - GLYPH_TMP_SIZE / 2,
            cy - GLYPH_TMP_SIZE / 2,
            cx + GLYPH_TMP_SIZE / 2 - 1,
            cy + GLYPH_TMP_SIZE / 2 - 1
        };

        // 3. Blit this glyph and FINISH the destination layer before the scratch buffer is overwritten.
        //    lv_draw_image() only queues a draw task that reads img_dsc.src (the shared scratch buffer) when it
        //    executes. lv_canvas_finish_layer() runs the queue synchronously, so finishing per glyph guarantees each
        //    glyph is drawn from its own pixels (queuing all glyphs and finishing once drew the last glyph everywhere).
        lv_layer_t dst_layer;
        lv_canvas_init_layer(cv, &dst_layer);
        lv_draw_image(&dst_layer, &img_dsc, &dst_area);
        lv_canvas_finish_layer(cv, &dst_layer);
    }
}

// --- Drawn vector icons (no font glyphs needed): AUTO gauge, LIGHT bulb, ANCHOR, USB, WARNING triangle ---
typedef enum { ICON_AUTO = 0, ICON_LIGHT, ICON_ANCHOR, ICON_USB, ICON_WARN } icon_id_t;

static lv_obj_t* icon_canvas_create(lv_obj_t *parent, int size, uint8_t **out_buf) {
    size_t bytes = (size_t)size * (size_t)size * 4U;
    *out_buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*out_buf) *out_buf = (uint8_t *)lv_malloc(bytes);
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, *out_buf, size, size, LV_COLOR_FORMAT_ARGB8888);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

static void icon_line(lv_layer_t *l, float x1, float y1, float x2, float y2, int w, lv_color_t c) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = x1; d.p1.y = y1;
    d.p2.x = x2; d.p2.y = y2;
    lv_draw_line(l, &d);
}

static void icon_arc(lv_layer_t *l, int cx, int cy, int r, int a0, int a1, int w, lv_color_t c) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = c;
    d.width = w;
    d.center.x = cx; d.center.y = cy;
    d.radius = (uint16_t)r;
    d.start_angle = a0;
    d.end_angle = a1;
    d.rounded = 1;
    lv_draw_arc(l, &d);
}

static void icon_circle(lv_layer_t *l, int cx, int cy, int r, int w, bool fill, lv_color_t c) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    if (fill) {
        d.bg_color = c; d.bg_opa = LV_OPA_COVER; d.border_width = 0;
    } else {
        d.bg_opa = LV_OPA_TRANSP; d.border_width = w; d.border_color = c; d.border_opa = LV_OPA_COVER;
    }
    lv_area_t a = { cx - r, cy - r, cx + r, cy + r };
    lv_draw_rect(l, &d, &a);
}

// Icons are designed on a 32 x 32 grid and scaled to `size`.
static void icon_draw(lv_obj_t *cv, icon_id_t id, int size, lv_color_t col) {
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);

    const float s = (float)size / 32.0f;
    int w = (int)(2.4f * s + 0.5f);
    if (w < 2) w = 2;
    #define IP(v) ((float)(v) * s)
    #define II(v) ((int)((float)(v) * s + 0.5f))

    switch (id) {
    case ICON_AUTO:
        icon_arc(&layer, II(16), II(17), II(12), 135, 45, w, col);
        icon_line(&layer, IP(16), IP(17), IP(22), IP(9), w, col);
        icon_circle(&layer, II(16), II(17), II(2.5f), 0, true, col);
        break;
    case ICON_LIGHT:
        icon_circle(&layer, II(16), II(13), II(8), w, false, col);
        icon_line(&layer, IP(12), IP(24), IP(20), IP(24), w, col);
        icon_line(&layer, IP(13), IP(28), IP(19), IP(28), w, col);
        icon_line(&layer, IP(16), IP(1.5f), IP(16), IP(3), w, col);
        icon_line(&layer, IP(3), IP(13), IP(5), IP(13), w, col);
        icon_line(&layer, IP(27), IP(13), IP(29), IP(13), w, col);
        break;
    case ICON_ANCHOR:
        icon_circle(&layer, II(16), II(6), II(3), w, false, col);
        icon_line(&layer, IP(16), IP(9), IP(16), IP(26), w, col);
        icon_line(&layer, IP(10), IP(13), IP(22), IP(13), w, col);
        icon_arc(&layer, II(16), II(17), II(10), 20, 160, w, col);
        icon_line(&layer, IP(6), IP(17), IP(7.5f), IP(21), w, col);
        icon_line(&layer, IP(26), IP(17), IP(24.5f), IP(21), w, col);
        break;
    case ICON_USB:
        icon_line(&layer, IP(16), IP(6), IP(16), IP(24), w, col);
        icon_circle(&layer, II(16), II(27), II(3), w, false, col);
        icon_line(&layer, IP(13), IP(8), IP(16), IP(3), w, col);
        icon_line(&layer, IP(16), IP(3), IP(19), IP(8), w, col);
        icon_line(&layer, IP(19), IP(8), IP(13), IP(8), w, col);
        icon_line(&layer, IP(16), IP(18), IP(9), IP(14), w, col);
        icon_line(&layer, IP(9), IP(14), IP(9), IP(11), w, col);
        icon_line(&layer, IP(16), IP(14), IP(23), IP(11), w, col);
        icon_circle(&layer, II(23), II(8), II(2.2f), 0, true, col);
        break;
    case ICON_WARN:
        icon_line(&layer, IP(16), IP(3), IP(29), IP(27), w, col);
        icon_line(&layer, IP(29), IP(27), IP(3), IP(27), w, col);
        icon_line(&layer, IP(3), IP(27), IP(16), IP(3), w, col);
        icon_line(&layer, IP(16), IP(11), IP(16), IP(19), w, col);
        icon_circle(&layer, II(16), II(23), II(1.6f), 0, true, col);
        break;
    }
    #undef IP
    #undef II
    lv_canvas_finish_layer(cv, &layer);
}

// Redraw an icon only when its colour changed (palette switch), never every refresh.
static void icon_recolor(lv_obj_t *cv, icon_id_t id, int size, lv_color_t col, uint32_t *cache) {
    uint32_t key = lv_color_to_u32(col) & 0x00FFFFFFu;
    if (*cache == key) return;
    *cache = key;
    icon_draw(cv, id, size, col);
}

// --- Idle & Power Management ---
static bool note_user_activity(bool is_alarm_ack) {
    uint8_t prev = current_backlight_pct;
    last_input_ms = snap.now_ms;
    if (current_backlight_pct != 100) {
        current_backlight_pct = 100;
        emit_action(active_mode, MARINE_ACT_POWER_SET_BACKLIGHT, 100, 0);
    }
    if (prev == 0 && !is_alarm_ack) {
        return false;
    }
    return true;
}

// --- Mode Ring Helpers (Including Live Shrunk Center Preview) ---
static void refresh_mode_ring_center_preview(void) {
    char top_b[24], mid_b[24], bot_b[24];
    if (active_mode == MARINE_MODE_AUTOPILOT) {
        snprintf(top_b, sizeof(top_b), "%03d°", (int)roundf(wrap_deg(snap.heading_deg)));
        snprintf(mid_b, sizeof(mid_b), "%s", snap.ap_engaged ? "AUTO" : "STANDBY");
        snprintf(bot_b, sizeof(bot_b), "CTS %03d°", (int)roundf(wrap_deg(snap.cts_deg)));
    } else if (active_mode == MARINE_MODE_ANCHOR_WATCH) {
        snprintf(top_b, sizeof(top_b), "%s", snap.anchor_armed ? "ANCHORED" : "READY");
        snprintf(mid_b, sizeof(mid_b), "%dm / %um", (int)roundf(snap.anchor_dist_m), snap.anchor_radius_m);
        snprintf(bot_b, sizeof(bot_b), "%s", snap.gps_3d_fix ? "3D FIX" : "NO FIX");
    } else if (active_mode == MARINE_MODE_DIMMER) {
        uint8_t idx = dimmer_selected_group < snap.dimmer_group_count ? dimmer_selected_group : 0;
        const marine_dimmer_group_t *g = &snap.dimmer_groups[idx];
        snprintf(top_b, sizeof(top_b), "%s", g->name[0] ? g->name : "LIGHT");
        if (g->on && g->level_pct > 0) snprintf(mid_b, sizeof(mid_b), "%u%%", (unsigned)g->level_pct);
        else snprintf(mid_b, sizeof(mid_b), "OFF");
        snprintf(bot_b, sizeof(bot_b), "GRP %u/%u", (unsigned)(idx + 1), (unsigned)(snap.dimmer_group_count ? snap.dimmer_group_count : 1));
    } else {
        snprintf(top_b, sizeof(top_b), "USB-HID");
        snprintf(mid_b, sizeof(mid_b), "%s", snap.usb_host_enumerated ? "ACTIVE" : "NO HOST");
        snprintf(bot_b, sizeof(bot_b), "2x MODES");
    }
    lv_label_set_text(ring_center_top, top_b);
    lv_label_set_text(ring_center_mid, mid_b);
    lv_label_set_text(ring_center_bot, bot_b);
    lv_obj_set_style_text_color(ring_center_top, c_muted(), 0);
    lv_obj_set_style_text_color(ring_center_mid, c_primary(), 0);
    lv_obj_set_style_text_color(ring_center_bot, c_text(), 0);
}

static void refresh_mode_ring(void) {
    refresh_mode_ring_center_preview();
    for (int s = 0; s < 4; s++) {
        marine_ui_mode_t m = ring_slot_modes[s];
        bool hidden = (m == MARINE_MODE_USB_HID && !snap.usb_host_enumerated);
        if (hidden) {
            lv_obj_add_flag(ring_slot_boxes[s], LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ring_slot_dots[s], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(ring_slot_boxes[s], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ring_slot_dots[s], LV_OBJ_FLAG_HIDDEN);

        bool sel = (s == ring_selected_slot);
        lv_obj_set_style_border_width(ring_slot_boxes[s], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_bg_opa(ring_slot_boxes[s], sel ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_shadow_width(ring_slot_boxes[s], (sel && snap.fx_glow_enabled) ? 16 : 0, 0);
        lv_obj_set_style_shadow_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_text_color(ring_slot_labels[s], sel ? c_primary() : c_text(), 0);
        static const icon_id_t slot_icons[4] = { ICON_AUTO, ICON_LIGHT, ICON_ANCHOR, ICON_USB };
        icon_recolor(ring_slot_icons[s], slot_icons[s], 28, sel ? c_primary() : c_text(), &ring_icon_col[s]);
    }
}

static void open_mode_ring(void) {
    mode_ring_open = true;
    ring_opened_ms = snap.now_ms;
    for (uint8_t s = 0; s < 4; s++) {
        if (ring_slot_modes[s] == active_mode) {
            ring_selected_slot = s;
            break;
        }
    }
    lv_obj_remove_flag(ovl_mode_ring, LV_OBJ_FLAG_HIDDEN);
    refresh_mode_ring();
    update_stale_overlay();   // the stale overlay never covers the mode ring
}

static void close_mode_ring(bool commit) {
    mode_ring_open = false;
    lv_obj_add_flag(ovl_mode_ring, LV_OBJ_FLAG_HIDDEN);
    if (commit) {
        active_mode = ring_slot_modes[ring_selected_slot];
        anchor_preview_active = false;
        cancel_transients();
        lv_screen_load_anim(scr_modes[active_mode], LV_SCR_LOAD_ANIM_FADE_IN, 120, 0, false);
        mode_registry[active_mode].refresh();
    }
    update_stale_overlay();   // stale state depends on the mode that is now active
}

// Depth, voltage and GPS fix line. A stale value shows `---` in its own place, the others stay.
static void format_telemetry(char *out, size_t n, uint32_t dbits, bool long_fix) {
    char d[16], v[16];
    const char *g;
    if (dbits & SB_DEPTH) snprintf(d, sizeof(d), "---m"); else snprintf(d, sizeof(d), "%.1fm", snap.depth_m);
    if (dbits & SB_VOLTAGE) snprintf(v, sizeof(v), "---V"); else snprintf(v, sizeof(v), "%.1fV", snap.voltage_v);
    if (dbits & SB_GPS) g = "---";
    else g = snap.gps_3d_fix ? (long_fix ? "3D FIX" : "3D") : "NO FIX";
    snprintf(out, n, "%s • %s • %s", d, v, g);
}

// --- Screen 1: Autopilot ---
static lv_obj_t* build_ap_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_radial_tick_ring(scr, ap_tick_lines, ap_tick_pts);

    ap_cts_bug = lv_obj_create(scr);
    lv_obj_set_size(ap_cts_bug, 10, 10);
    lv_obj_set_style_radius(ap_cts_bug, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ap_cts_bug, 0, 0);
    lv_obj_set_style_bg_color(ap_cts_bug, COL_CYAN, 0);

    lv_obj_t *lbl_hdr = lv_label_create(scr);
    lv_label_set_text(lbl_hdr, "HEADING");
    lv_obj_set_style_text_font(lbl_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hdr, COL_TEXT_MUTED, 0);
    lv_obj_align(lbl_hdr, LV_ALIGN_TOP_MID, 0, 26);

    ap_lbl_hdg = lv_label_create(scr);
    lv_label_set_text(ap_lbl_hdg, "---");
    lv_obj_set_style_text_font(ap_lbl_hdg, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(ap_lbl_hdg, COL_TEXT_MAIN, 0);
    lv_obj_align(ap_lbl_hdg, LV_ALIGN_TOP_MID, 0, 42);

    ap_pill_mode = lv_obj_create(scr);
    lv_obj_set_size(ap_pill_mode, 148, 22);
    lv_obj_align(ap_pill_mode, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_radius(ap_pill_mode, 11, 0);
    lv_obj_set_style_bg_color(ap_pill_mode, COL_CARD, 0);
    lv_obj_set_style_border_width(ap_pill_mode, 1, 0);
    lv_obj_remove_flag(ap_pill_mode, LV_OBJ_FLAG_SCROLLABLE);

    ap_lbl_mode = lv_label_create(ap_pill_mode);
    lv_label_set_text(ap_lbl_mode, "STANDBY");
    lv_obj_set_style_text_font(ap_lbl_mode, &lv_font_montserrat_12, 0);
    lv_obj_center(ap_lbl_mode);

    ap_lbl_cts = lv_label_create(scr);
    lv_label_set_text(ap_lbl_cts, "CTS ---");
    lv_obj_set_style_text_font(ap_lbl_cts, &lv_font_montserrat_28, 0);
    lv_obj_align(ap_lbl_cts, LV_ALIGN_CENTER, 0, 14);

    ap_lbl_delta = lv_label_create(scr);
    lv_label_set_text(ap_lbl_delta, "DELTA ---");
    lv_obj_set_style_text_font(ap_lbl_delta, &lv_font_montserrat_12, 0);
    lv_obj_align(ap_lbl_delta, LV_ALIGN_CENTER, 0, 36);

    lv_obj_t *rud_bg = lv_obj_create(scr);
    lv_obj_set_size(rud_bg, 116, 8);
    lv_obj_align(rud_bg, LV_ALIGN_BOTTOM_MID, 0, -54);
    lv_obj_set_style_bg_color(rud_bg, COL_CARD, 0);
    lv_obj_set_style_border_width(rud_bg, 1, 0);
    lv_obj_set_style_border_color(rud_bg, lv_color_hex(0x233856), 0);
    lv_obj_remove_flag(rud_bg, LV_OBJ_FLAG_SCROLLABLE);

    ap_rudder_fill = lv_obj_create(rud_bg);
    lv_obj_set_size(ap_rudder_fill, 10, 6);
    lv_obj_set_style_border_width(ap_rudder_fill, 0, 0);

    ap_lbl_telem = lv_label_create(scr);
    lv_label_set_text(ap_lbl_telem, "---m • ---V • ---");
    lv_obj_set_style_text_font(ap_lbl_telem, &lv_font_montserrat_12, 0);
    lv_obj_align(ap_lbl_telem, LV_ALIGN_BOTTOM_MID, 0, -34);

    return scr;
}

static void ap_on_input(marine_semantic_input_t ev, int32_t val) {
    if (ev == MARINE_INPUT_TURN && snap.ap_engaged) {
        int32_t base = ap_pending_active ? ap_pending_cts : (int32_t)roundf(snap.cts_deg);
        ap_pending_cts = (int32_t)wrap_deg((float)(base + val * ap_step_deg));
        ap_pending_active = true;
        ap_pending_sent = false;
        ap_pending_deadline_ms = snap.now_ms + 400; // 400 ms pause before commit
        ap_refresh();
    } else if (ev == MARINE_INPUT_SINGLE_PRESS && snap.ap_engaged) {
        ap_step_deg = (ap_step_deg == 1) ? 10 : 1;
        ap_refresh();
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        if (!snap.ap_engaged) {
            ap_confirm_open = true;
            ap_confirm_deadline_ms = snap.now_ms + 5000;
            lv_obj_remove_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        } else {
            ap_pending_active = false;
            ap_pending_sent = false;
            emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_STANDBY, 1, 0);
        }
    }
}

static void ap_refresh(void) {
    char buf[40];
    bool mask = (current_stale() != STALE_NONE);   // essential values (or the link) stale: mask the whole mode
    uint32_t dbits = display_stale_bits();          // display-only values stale: mask just those
    int32_t target = ap_pending_active ? ap_pending_cts : (int32_t)roundf(wrap_deg(snap.cts_deg));

    ap_tick_ring_maybe_update();

    if (mask) {
        lv_label_set_text(ap_lbl_hdg, "---");
        lv_label_set_text(ap_lbl_mode, "---");
        lv_label_set_text(ap_lbl_cts, "CTS ---");
        lv_label_set_text(ap_lbl_delta, "DELTA ---");
        lv_label_set_text(ap_lbl_telem, "---m • ---V • ---");
        lv_obj_add_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);

    snprintf(buf, sizeof(buf), "%03d°", (int)roundf(wrap_deg(snap.heading_deg)));
    lv_label_set_text(ap_lbl_hdg, buf);
    lv_obj_set_style_text_color(ap_lbl_hdg, c_text(), 0);

    snprintf(buf, sizeof(buf), "CTS %03d°", (int)target);
    lv_label_set_text(ap_lbl_cts, buf);
    lv_obj_set_style_text_color(ap_lbl_cts, ap_pending_active ? c_amber() : c_primary(), 0);

    float diff = (float)target - snap.heading_deg;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;

    float bug_rad = (diff - 90.0f) * (float)(M_PI / 180.0);
    lv_obj_set_pos(ap_cts_bug, 120 + (int)(cosf(bug_rad) * 108.0f) - 5, 120 + (int)(sinf(bug_rad) * 108.0f) - 5);
    lv_obj_set_style_bg_color(ap_cts_bug, ap_pending_active ? c_amber() : c_primary(), 0);

    int abs_d = (int)roundf(fabsf(diff));
    if (abs_d == 0) lv_label_set_text(ap_lbl_delta, "ON COURSE");
    else {
        snprintf(buf, sizeof(buf), "DELTA %d° %s", abs_d, diff > 0 ? "STBD" : "PORT");
        lv_label_set_text(ap_lbl_delta, buf);
    }
    lv_obj_set_style_text_color(ap_lbl_delta, c_muted(), 0);

    if (snap.ap_engaged) {
        snprintf(buf, sizeof(buf), "AUTO • STEP %d°", ap_step_deg);
        lv_label_set_text(ap_lbl_mode, buf);
        lv_obj_set_style_text_color(ap_lbl_mode, c_primary(), 0);
        lv_obj_set_style_border_color(ap_pill_mode, c_primary(), 0);
    } else {
        lv_label_set_text(ap_lbl_mode, "STANDBY");
        lv_obj_set_style_text_color(ap_lbl_mode, c_amber(), 0);
        lv_obj_set_style_border_color(ap_pill_mode, c_amber(), 0);
    }

    if (dbits & SB_RUDDER) {
        lv_obj_add_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);   // stale rudder: hide the bar, nothing else changes
    } else {
        lv_obj_remove_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);
        int bw = (int)((fabsf(snap.rudder_deg) / 35.0f) * 54.0f);
        if (bw < 3) bw = 3;
        if (bw > 54) bw = 54;
        lv_obj_set_width(ap_rudder_fill, bw);
        lv_obj_align(ap_rudder_fill, LV_ALIGN_CENTER, snap.rudder_deg >= 0 ? (bw / 2) : -(bw / 2), 0);
        lv_obj_set_style_bg_color(ap_rudder_fill, snap.night_palette ? COL_NIGHT_RED : (snap.rudder_deg >= 0 ? COL_GREEN : COL_RED), 0);
    }

    format_telemetry(buf, sizeof(buf), dbits, false);
    lv_label_set_text(ap_lbl_telem, buf);
    lv_obj_set_style_text_color(ap_lbl_telem, c_muted(), 0);
}

// --- Screen 2: Anchor Watch ---
static lv_obj_t* build_anchor_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    anc_ring = lv_obj_create(scr);
    lv_obj_set_size(anc_ring, 144, 144);
    lv_obj_center(anc_ring);
    lv_obj_set_style_radius(anc_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(anc_ring, COL_CARD, 0);
    lv_obj_set_style_border_width(anc_ring, 2, 0);

    anc_lbl_center = lv_label_create(scr);
    lv_label_set_text(anc_lbl_center, "HOLD FOR ACTIONS");
    lv_obj_set_style_text_font(anc_lbl_center, &lv_font_montserrat_12, 0);
    lv_obj_align(anc_lbl_center, LV_ALIGN_CENTER, 0, 14);

    anc_boat_dot = lv_obj_create(scr);
    lv_obj_set_size(anc_boat_dot, 12, 12);
    lv_obj_set_style_radius(anc_boat_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(anc_boat_dot, COL_GREEN, 0);
    lv_obj_set_style_border_color(anc_boat_dot, COL_TEXT_MAIN, 0);
    lv_obj_set_style_border_width(anc_boat_dot, 2, 0);

    anc_canvas_top = create_curved_canvas(scr, LV_ALIGN_TOP_MID, &anc_buf_top);
    anc_canvas_bot = create_curved_canvas(scr, LV_ALIGN_BOTTOM_MID, &anc_buf_bot);
    return scr;
}

static void anchor_on_input(marine_semantic_input_t ev, int32_t val) {
    if (ev == MARINE_INPUT_TURN) {
        int32_t base = anchor_preview_active ? (int32_t)anchor_preview_radius : (int32_t)snap.anchor_radius_m;
        int32_t next = base + val * 5;
        if (next < 10) next = 10;
        if (next > 150) next = 150;
        anchor_preview_radius = (uint16_t)next;
        anchor_preview_active = true;
        anchor_refresh();
    } else if (ev == MARINE_INPUT_SINGLE_PRESS && anchor_preview_active) {
        uint16_t r = anchor_preview_radius;
        anchor_preview_active = false;
        emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_SET_RADIUS, r, 0);
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        anchor_menu_open = true;
        anchor_menu_idx = 0;
        anchor_menu_opened_ms = snap.now_ms;
        lv_label_set_text(anc_menu_items[0], snap.anchor_armed ? "MOVE DROP POINT HERE" : "ARM HERE");
        if (snap.anchor_armed) lv_obj_remove_flag(anc_menu_items[1], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(anc_menu_items[1], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anchor_refresh(void) {
    char top_buf[48], bot_buf[48];
    bool mask = (current_stale() != STALE_NONE);   // essential anchor data (or the link) stale: mask the mode
    uint32_t dbits = display_stale_bits();          // display-only values stale: mask just those
    uint16_t r = anchor_preview_active ? anchor_preview_radius : snap.anchor_radius_m;

    if (mask) {
        update_curved_canvas(anc_canvas_top, anc_cache_top, "--- / ---", false, c_muted());
        update_curved_canvas(anc_canvas_bot, anc_cache_bot, "---m • ---V • ---", true, c_muted());
        lv_obj_add_flag(anc_boat_dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (anchor_preview_active) {
        snprintf(top_buf, sizeof(top_buf), "SET RADIUS • %um", r);
        lv_label_set_text(anc_lbl_center, "PRESS TO APPLY");
        lv_obj_set_style_border_color(anc_ring, c_amber(), 0);
    } else if (snap.anchor_armed) {
        snprintf(top_buf, sizeof(top_buf), "ANCHORED • %dm / %um", (int)roundf(snap.anchor_dist_m), r);
        lv_label_set_text(anc_lbl_center, "HOLD FOR ACTIONS");
        lv_obj_set_style_border_color(anc_ring, c_primary(), 0);
    } else {
        snprintf(top_buf, sizeof(top_buf), "READY • %um", r);
        lv_label_set_text(anc_lbl_center, "HOLD TO ARM");
        lv_obj_set_style_border_color(anc_ring, c_muted(), 0);
    }
    lv_obj_set_style_text_color(anc_lbl_center, c_text(), 0);

    format_telemetry(bot_buf, sizeof(bot_buf), dbits, true);
    update_curved_canvas(anc_canvas_top, anc_cache_top, top_buf, false, anchor_preview_active ? c_amber() : c_primary());
    update_curved_canvas(anc_canvas_bot, anc_cache_bot, bot_buf, true, c_muted());

    if (snap.anchor_armed && snap.gps_3d_fix) {
        lv_obj_remove_flag(anc_boat_dot, LV_OBJ_FLAG_HIDDEN);
        float ratio = snap.anchor_dist_m / (float)r;
        if (ratio > 1.15f) ratio = 1.15f;
        float th = (snap.anchor_bearing_deg - 90.0f) * (float)(M_PI / 180.0);
        lv_obj_align(anc_boat_dot, LV_ALIGN_CENTER, (int)(cosf(th) * ratio * 68.0f), (int)(sinf(th) * ratio * 68.0f));
    } else {
        lv_obj_add_flag(anc_boat_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

// --- Screen 3: Dimmer (Buffered pending value across rapid turns + 7-preset RGBW variant) ---
static lv_obj_t* build_dimmer_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    dim_arc = lv_arc_create(scr);
    lv_obj_set_size(dim_arc, 224, 224);
    lv_obj_center(dim_arc);
    lv_arc_set_bg_angles(dim_arc, 135, 45);
    lv_arc_set_range(dim_arc, 0, 100);
    lv_obj_remove_style(dim_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(dim_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dim_arc, lv_color_hex(0x16263E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(dim_arc, 10, LV_PART_INDICATOR);
    lv_obj_remove_flag(dim_arc, LV_OBJ_FLAG_CLICKABLE);

    dim_rgbw_swatch = lv_obj_create(scr);
    lv_obj_set_size(dim_rgbw_swatch, 96, 96);
    lv_obj_center(dim_rgbw_swatch);
    lv_obj_set_style_radius(dim_rgbw_swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(dim_rgbw_swatch, 2, 0);
    lv_obj_add_flag(dim_rgbw_swatch, LV_OBJ_FLAG_HIDDEN);

    dim_lbl_grp_count = lv_label_create(scr);
    lv_obj_set_style_text_font(dim_lbl_grp_count, &lv_font_montserrat_12, 0);
    lv_obj_align(dim_lbl_grp_count, LV_ALIGN_TOP_MID, 0, 44);

    for (int i = 0; i < MARINE_MAX_DIMMER_GROUPS; i++) {
        dim_dots[i] = lv_obj_create(scr);
        lv_obj_set_style_radius(dim_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dim_dots[i], 0, 0);
    }

    dim_lbl_val = lv_label_create(scr);
    lv_obj_set_style_text_font(dim_lbl_val, &lv_font_montserrat_44, 0);
    lv_obj_align(dim_lbl_val, LV_ALIGN_CENTER, 0, -4);

    dim_lbl_name = lv_label_create(scr);
    lv_obj_set_style_text_font(dim_lbl_name, &lv_font_montserrat_14, 0);
    lv_obj_align(dim_lbl_name, LV_ALIGN_CENTER, 0, 28);

    dim_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(dim_lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(dim_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -42);

    return scr;
}

static void dimmer_on_input(marine_semantic_input_t ev, int32_t val) {
    uint8_t cnt = snap.dimmer_group_count > 0 ? snap.dimmer_group_count : 1;
    if (dimmer_selected_group >= cnt) dimmer_selected_group = 0;

    if (snap.dimmer_use_rgbw_variant) {
        if (ev == MARINE_INPUT_TURN) {
            uint8_t base = dimmer_pending_active ? dimmer_pending_preset : (snap.rgbw_preset_idx % MARINE_RGBW_PRESET_COUNT);
            int next = ((int)base + val + MARINE_RGBW_PRESET_COUNT * 4) % MARINE_RGBW_PRESET_COUNT;
            dimmer_pending_preset = (uint8_t)next;
            dimmer_pending_active = true;
            dimmer_pending_sent = false;
            dimmer_pending_deadline_ms = snap.now_ms + 250;
            dimmer_refresh();
        }
        return;
    }

    if (ev == MARINE_INPUT_TURN) {
        const marine_dimmer_group_t *grp = &snap.dimmer_groups[dimmer_selected_group];
        if (dimmer_wake_active && dimmer_wake_group == dimmer_selected_group) return; // switch-on in flight, swallow
        if (!dimmer_pending_active && !grp->on && grp->level_pct > 0) {
            // Off with a saved dim level: this click only turns the light back on, it does not change the level.
            dimmer_wake_active = true;
            dimmer_wake_group = dimmer_selected_group;
            dimmer_wake_deadline_ms = snap.now_ms + MARINE_PENDING_CONFIRM_TIMEOUT;
            emit_action(MARINE_MODE_DIMMER, MARINE_ACT_DIMMER_TOGGLE_POWER, dimmer_selected_group, 0);
            return;
        }
        uint8_t base = dimmer_pending_active ? dimmer_pending_level : snap.dimmer_groups[dimmer_selected_group].level_pct;
        int next_lvl = (int)base + (val * 5);
        if (next_lvl < 0) next_lvl = 0;
        if (next_lvl > 100) next_lvl = 100;
        dimmer_pending_level = (uint8_t)next_lvl;
        dimmer_pending_active = true;
        dimmer_pending_sent = false;
        dimmer_pending_deadline_ms = snap.now_ms + 250;
        dimmer_refresh();
    } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
        dimmer_pending_active = false;
        dimmer_selected_group = (dimmer_selected_group + 1) % cnt;
        dimmer_refresh();
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        dimmer_pending_active = false;
        emit_action(MARINE_MODE_DIMMER, MARINE_ACT_DIMMER_TOGGLE_POWER, dimmer_selected_group, 0);
    }
}

static void dimmer_refresh(void) {
    bool mask = (current_stale() != STALE_NONE);
    if (snap.dimmer_use_rgbw_variant) {
        lv_obj_add_flag(dim_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dim_lbl_val, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(dim_rgbw_swatch, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < MARINE_MAX_DIMMER_GROUPS; i++) lv_obj_add_flag(dim_dots[i], LV_OBJ_FLAG_HIDDEN);

        uint8_t p_idx = dimmer_pending_active ? dimmer_pending_preset : (snap.rgbw_preset_idx % MARINE_RGBW_PRESET_COUNT);
        lv_color_t sc = snap.night_palette ? COL_NIGHT_RED : lv_color_hex(rgbw_preset_hex[p_idx]);
        lv_obj_set_style_bg_color(dim_rgbw_swatch, sc, 0);
        lv_label_set_text(dim_lbl_grp_count, "RGBW PRESET (1..7)");
        lv_label_set_text(dim_lbl_name, mask ? "---" : rgbw_preset_names[p_idx]);
        lv_obj_align(dim_lbl_name, LV_ALIGN_BOTTOM_MID, 0, -56);
        lv_label_set_text(dim_lbl_hint, "TURN: CYCLE 7 PRESETS");
        return;
    }

    lv_obj_remove_flag(dim_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(dim_lbl_val, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dim_rgbw_swatch, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(dim_lbl_name, LV_ALIGN_CENTER, 0, 28);

    uint8_t cnt = snap.dimmer_group_count > 0 ? snap.dimmer_group_count : 1;
    if (dimmer_selected_group >= cnt) dimmer_selected_group = 0;
    const marine_dimmer_group_t *g = &snap.dimmer_groups[dimmer_selected_group];
    uint8_t lvl = dimmer_pending_active ? dimmer_pending_level : g->level_pct;
    bool is_on = !mask && (dimmer_pending_active ? (lvl > 0) : (g->on && lvl > 0));

    lv_arc_set_value(dim_arc, is_on ? lvl : 0);
    lv_obj_set_style_arc_color(dim_arc, dimmer_pending_active ? c_amber() : (snap.night_palette ? COL_NIGHT_RED : COL_GOLD), LV_PART_INDICATOR);

    char buf[32];
    snprintf(buf, sizeof(buf), "GROUP %u OF %u", (unsigned)(dimmer_selected_group + 1), (unsigned)cnt);
    lv_label_set_text(dim_lbl_grp_count, buf);
    lv_obj_set_style_text_color(dim_lbl_grp_count, c_muted(), 0);

    int spacing = 16;
    int start_x = -((cnt - 1) * spacing) / 2;
    for (int i = 0; i < MARINE_MAX_DIMMER_GROUPS; i++) {
        if (i >= cnt) {
            lv_obj_add_flag(dim_dots[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(dim_dots[i], LV_OBJ_FLAG_HIDDEN);
        bool sel = (i == dimmer_selected_group);
        lv_obj_set_size(dim_dots[i], sel ? 10 : 6, sel ? 10 : 6);
        lv_obj_set_style_bg_color(dim_dots[i], sel ? c_primary() : c_muted(), 0);
        lv_obj_align(dim_dots[i], LV_ALIGN_TOP_MID, start_x + i * spacing, 64);
    }

    if (mask) {
        lv_label_set_text(dim_lbl_val, "---");
    } else if (is_on) {
        snprintf(buf, sizeof(buf), "%u%%", (unsigned)lvl);
        lv_label_set_text(dim_lbl_val, buf);
        lv_label_set_text(dim_lbl_hint, "1x: GROUP • HOLD: OFF");
    } else {
        lv_label_set_text(dim_lbl_val, "OFF");
        lv_label_set_text(dim_lbl_hint, "HOLD TO TURN ON");
    }
    lv_obj_set_style_text_color(dim_lbl_val, c_text(), 0);
    lv_label_set_text(dim_lbl_name, g->name[0] ? g->name : "GROUP");
    lv_obj_set_style_text_color(dim_lbl_name, is_on ? c_primary() : c_muted(), 0);
    lv_obj_set_style_text_color(dim_lbl_hint, c_muted(), 0);
}

// --- Screen 4: USB-HID (No Gauge Arc; live SignalK link status) ---
static const char *hid_names[HID_PROFILE_COUNT] = { "CHARTPLOTTER", "MEDIA / AUDIO", "CURSOR KEYS" };

static lv_obj_t* build_hid_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    hid_lbl_status = lv_label_create(scr);
    lv_obj_set_style_text_font(hid_lbl_status, &lv_font_montserrat_12, 0);
    lv_obj_align(hid_lbl_status, LV_ALIGN_TOP_MID, 0, 56);

    hid_lbl_profile = lv_label_create(scr);
    lv_obj_set_style_text_font(hid_lbl_profile, &lv_font_montserrat_28, 0);
    lv_obj_align(hid_lbl_profile, LV_ALIGN_CENTER, 0, -4);

    hid_lbl_sub = lv_label_create(scr);
    lv_obj_set_style_text_font(hid_lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_align(hid_lbl_sub, LV_ALIGN_CENTER, 0, 26);

    hid_lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(hid_lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_align(hid_lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -48);

    return scr;
}

static void hid_on_input(marine_semantic_input_t ev, int32_t val) {
    if (!snap.usb_host_enumerated) return;
    if (ev == MARINE_INPUT_TURN) emit_action(MARINE_MODE_USB_HID, MARINE_ACT_HID_TURN, val, snap.hid_profile);
    else if (ev == MARINE_INPUT_SINGLE_PRESS) emit_action(MARINE_MODE_USB_HID, MARINE_ACT_HID_PRESS, 1, snap.hid_profile);
    else if (ev == MARINE_INPUT_LONG_PRESS) emit_action(MARINE_MODE_USB_HID, MARINE_ACT_HID_NEXT_PROFILE, 1, snap.hid_profile);
}

static void hid_refresh(void) {
    if (!snap.usb_host_enumerated) {
        lv_label_set_text(hid_lbl_status, "USB DISCONNECTED");
        lv_obj_set_style_text_color(hid_lbl_status, c_red(), 0);
        lv_label_set_text(hid_lbl_profile, "NO USB HOST");
        lv_obj_set_style_text_color(hid_lbl_profile, c_text(), 0);
        lv_label_set_text(hid_lbl_sub, snap.signalk_connected ? "SIGNALK LINK STILL UP" : "SIGNALK LINK DOWN");
        lv_obj_set_style_text_color(hid_lbl_sub, snap.night_palette ? COL_NIGHT_RED : (snap.signalk_connected ? COL_GREEN : COL_AMBER), 0);
        lv_label_set_text(hid_lbl_hint, "2x PRESS: MODES");
        lv_obj_set_style_text_color(hid_lbl_hint, c_muted(), 0);
        return;
    }
    lv_label_set_text(hid_lbl_status, "USB HOST CONNECTED");
    lv_obj_set_style_text_color(hid_lbl_status, snap.night_palette ? COL_NIGHT_RED : COL_GREEN, 0);
    lv_label_set_text(hid_lbl_profile, hid_names[snap.hid_profile % HID_PROFILE_COUNT]);
    lv_obj_set_style_text_color(hid_lbl_profile, c_text(), 0);
    lv_label_set_text(hid_lbl_sub, "TURN: STEP • 1x: CLICK");
    lv_obj_set_style_text_color(hid_lbl_sub, snap.night_palette ? COL_NIGHT_RED : COL_PURPLE, 0);
    lv_label_set_text(hid_lbl_hint, "HOLD: NEXT PROFILE");
    lv_obj_set_style_text_color(hid_lbl_hint, c_muted(), 0);
}

// --- Overlays: Mode Ring, Alarm Takeover, Ack Badge, Stale Link, Prompts & Styled System Screen ---
static void build_overlays_and_system(void) {
    // 1. Glass Mode Ring with Live Center Preview
    ovl_mode_ring = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_mode_ring, 240, 240);
    lv_obj_center(ovl_mode_ring);
    lv_obj_set_style_radius(ovl_mode_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_mode_ring, lv_color_hex(0x040A14), 0);
    lv_obj_set_style_bg_opa(ovl_mode_ring, LV_OPA_80, 0);
    lv_obj_set_style_border_width(ovl_mode_ring, 2, 0);
    lv_obj_set_style_border_color(ovl_mode_ring, lv_color_hex(0x6E86A6), 0);
    lv_obj_remove_flag(ovl_mode_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_mode_ring, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *center_cut = lv_obj_create(ovl_mode_ring);
    lv_obj_set_size(center_cut, 112, 112);
    lv_obj_center(center_cut);
    lv_obj_set_style_radius(center_cut, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_cut, COL_BG, 0);
    lv_obj_set_style_border_width(center_cut, 1, 0);
    lv_obj_set_style_border_color(center_cut, lv_color_hex(0x3D5270), 0);
    lv_obj_remove_flag(center_cut, LV_OBJ_FLAG_SCROLLABLE);

    ring_center_top = lv_label_create(center_cut);
    lv_obj_set_style_text_font(ring_center_top, &lv_font_montserrat_12, 0);
    lv_obj_align(ring_center_top, LV_ALIGN_TOP_MID, 0, 14);

    ring_center_mid = lv_label_create(center_cut);
    lv_obj_set_style_text_font(ring_center_mid, &lv_font_montserrat_14, 0);
    lv_obj_align(ring_center_mid, LV_ALIGN_CENTER, 0, 0);

    ring_center_bot = lv_label_create(center_cut);
    lv_obj_set_style_text_font(ring_center_bot, &lv_font_montserrat_12, 0);
    lv_obj_align(ring_center_bot, LV_ALIGN_BOTTOM_MID, 0, -14);

    const lv_coord_t slot_dx[4] = { 0, 86, 0, -86 };
    const lv_coord_t slot_dy[4] = { -86, 0, 86, 0 };
    for (int s = 0; s < 4; s++) {
        ring_slot_boxes[s] = lv_obj_create(ovl_mode_ring);
        lv_obj_set_size(ring_slot_boxes[s], 54, 48);          // drawn icon (28 px) above the label
        lv_obj_set_style_radius(ring_slot_boxes[s], 6, 0);
        lv_obj_set_style_pad_all(ring_slot_boxes[s], 0, 0);
        lv_obj_align(ring_slot_boxes[s], LV_ALIGN_CENTER, slot_dx[s], slot_dy[s]);
        lv_obj_remove_flag(ring_slot_boxes[s], LV_OBJ_FLAG_SCROLLABLE);

        ring_slot_icons[s] = icon_canvas_create(ring_slot_boxes[s], 28, &ring_icon_buf[s]);
        lv_obj_align(ring_slot_icons[s], LV_ALIGN_TOP_MID, 0, 3);

        ring_slot_labels[s] = lv_label_create(ring_slot_boxes[s]);
        lv_label_set_text(ring_slot_labels[s], mode_registry[ring_slot_modes[s]].ring_label);
        lv_obj_set_style_text_font(ring_slot_labels[s], &lv_font_montserrat_12, 0);
        lv_obj_align(ring_slot_labels[s], LV_ALIGN_BOTTOM_MID, 0, -2);

        ring_slot_dots[s] = lv_obj_create(ovl_mode_ring);
        lv_obj_set_size(ring_slot_dots[s], 8, 8);
        lv_obj_set_style_radius(ring_slot_dots[s], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring_slot_dots[s], COL_TEXT_MUTED, 0);
        lv_obj_set_style_bg_opa(ring_slot_dots[s], LV_OPA_30, 0);
        lv_obj_set_style_border_width(ring_slot_dots[s], 0, 0);
        lv_obj_align(ring_slot_dots[s], LV_ALIGN_CENTER, slot_dx[s], slot_dy[s]);
        lv_obj_add_flag(ring_slot_dots[s], LV_OBJ_FLAG_HIDDEN);
    }

    // 2. Full-Screen Alarm Takeover
    ovl_alarm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_alarm, 240, 240);
    lv_obj_center(ovl_alarm);
    lv_obj_set_style_radius(ovl_alarm, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_alarm, COL_BG, 0);
    lv_obj_set_style_border_width(ovl_alarm, 8, 0);
    lv_obj_set_style_border_color(ovl_alarm, lv_color_hex(0x4A5668), 0);
    lv_obj_remove_flag(ovl_alarm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_alarm, LV_OBJ_FLAG_HIDDEN);

    alm_geofence = lv_obj_create(ovl_alarm);
    lv_obj_set_size(alm_geofence, 84, 84);
    lv_obj_align(alm_geofence, LV_ALIGN_CENTER, 0, -34);
    lv_obj_set_style_radius(alm_geofence, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(alm_geofence, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(alm_geofence, 2, 0);
    lv_obj_set_style_border_color(alm_geofence, COL_TEXT_MUTED, 0);
    lv_obj_remove_flag(alm_geofence, LV_OBJ_FLAG_SCROLLABLE);

    // Drawn anchor icon inside the geofence and a drawn warning triangle above the headline (no font glyphs)
    alm_icon_anchor = icon_canvas_create(ovl_alarm, 48, &alm_icon_anchor_buf);
    lv_obj_align(alm_icon_anchor, LV_ALIGN_CENTER, 0, -34);
    alm_icon_warn = icon_canvas_create(ovl_alarm, 22, &alm_icon_warn_buf);
    lv_obj_align(alm_icon_warn, LV_ALIGN_CENTER, 0, 14);

    alm_boat_dot = lv_obj_create(ovl_alarm);
    lv_obj_set_size(alm_boat_dot, 12, 12);
    lv_obj_set_style_radius(alm_boat_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(alm_boat_dot, COL_RED, 0);
    lv_obj_set_style_border_color(alm_boat_dot, COL_TEXT_MAIN, 0);
    lv_obj_set_style_border_width(alm_boat_dot, 1, 0);
    lv_obj_align(alm_boat_dot, LV_ALIGN_CENTER, 44, -66);   // outside the geofence circle (distance ~54 px > 42 px)

    alm_lbl_title = lv_label_create(ovl_alarm);
    lv_label_set_text(alm_lbl_title, "DRAG ALARM!");
    lv_obj_set_style_text_font(alm_lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(alm_lbl_title, COL_RED, 0);
    lv_obj_align(alm_lbl_title, LV_ALIGN_CENTER, 0, 40);

    alm_lbl_ack = lv_label_create(ovl_alarm);
    lv_label_set_text(alm_lbl_ack, "PRESS TO ACKNOWLEDGE");
    lv_obj_set_style_text_font(alm_lbl_ack, &lv_font_montserrat_12, 0);
    lv_obj_align(alm_lbl_ack, LV_ALIGN_CENTER, 0, 66);

    alm_canvas_top = create_curved_canvas(ovl_alarm, LV_ALIGN_TOP_MID, &alm_buf_top);
    alm_canvas_bot = create_curved_canvas(ovl_alarm, LV_ALIGN_BOTTOM_MID, &alm_buf_bot);

    // 2b. Persistent Acknowledged Alarm Ring & Label (when alarm_ack_return_with_badge == true)
    ovl_alarm_ack_ring = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_alarm_ack_ring, 240, 240);
    lv_obj_center(ovl_alarm_ack_ring);
    lv_obj_set_style_radius(ovl_alarm_ack_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ovl_alarm_ack_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ovl_alarm_ack_ring, 4, 0);
    lv_obj_set_style_border_color(ovl_alarm_ack_ring, COL_RED, 0);
    lv_obj_remove_flag(ovl_alarm_ack_ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_alarm_ack_ring, LV_OBJ_FLAG_HIDDEN);

    ovl_alarm_ack_badge = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_alarm_ack_badge, 108, 18);
    lv_obj_align(ovl_alarm_ack_badge, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(ovl_alarm_ack_badge, 9, 0);
    lv_obj_set_style_bg_color(ovl_alarm_ack_badge, COL_RED, 0);
    lv_obj_set_style_border_width(ovl_alarm_ack_badge, 0, 0);
    lv_obj_remove_flag(ovl_alarm_ack_badge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_alarm_ack_badge, LV_OBJ_FLAG_HIDDEN);

    ovl_alarm_ack_lbl = lv_label_create(ovl_alarm_ack_badge);
    lv_obj_set_style_text_font(ovl_alarm_ack_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ovl_alarm_ack_lbl, COL_TEXT_MAIN, 0);
    lv_obj_center(ovl_alarm_ack_lbl);

    // 3. Stale Link Overlay
    ovl_stale = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_stale, 240, 240);
    lv_obj_center(ovl_stale);
    lv_obj_set_style_radius(ovl_stale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_stale, lv_color_hex(0x02050B), 0);
    lv_obj_set_style_bg_opa(ovl_stale, LV_OPA_80, 0);
    lv_obj_set_style_border_width(ovl_stale, 5, 0);
    lv_obj_set_style_border_color(ovl_stale, COL_AMBER, 0);
    lv_obj_remove_flag(ovl_stale, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_stale, LV_OBJ_FLAG_HIDDEN);

    // Texts are set by update_stale_overlay(): LINK LOST + age, or the reason of a stale essential value.
    stale_lbl_title = lv_label_create(ovl_stale);
    lv_label_set_text(stale_lbl_title, "LINK LOST");
    lv_obj_set_style_text_font(stale_lbl_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(stale_lbl_title, COL_AMBER, 0);
    lv_obj_align(stale_lbl_title, LV_ALIGN_CENTER, 0, -22);

    stale_lbl_age = lv_label_create(ovl_stale);
    lv_label_set_text(stale_lbl_age, "");
    lv_obj_set_style_text_font(stale_lbl_age, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(stale_lbl_age, COL_TEXT_MAIN, 0);
    lv_obj_align(stale_lbl_age, LV_ALIGN_CENTER, 0, 6);

    stale_lbl_sub = lv_label_create(ovl_stale);
    lv_label_set_text(stale_lbl_sub, "");
    lv_obj_set_style_text_font(stale_lbl_sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(stale_lbl_sub, COL_TEXT_MUTED, 0);
    lv_obj_align(stale_lbl_sub, LV_ALIGN_CENTER, 0, 32);

    // 4. Autopilot Engage Confirm Prompt
    ovl_ap_confirm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_ap_confirm, 240, 240);
    lv_obj_center(ovl_ap_confirm);
    lv_obj_set_style_radius(ovl_ap_confirm, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_ap_confirm, COL_BG, 0);
    lv_obj_set_style_bg_opa(ovl_ap_confirm, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ovl_ap_confirm, 0, 0);
    lv_obj_remove_flag(ovl_ap_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);

    ap_confirm_arc = lv_arc_create(ovl_ap_confirm);
    lv_obj_set_size(ap_confirm_arc, 224, 224);
    lv_obj_center(ap_confirm_arc);
    lv_arc_set_rotation(ap_confirm_arc, 270);
    lv_arc_set_bg_angles(ap_confirm_arc, 0, 360);
    lv_arc_set_range(ap_confirm_arc, 0, 5000);
    lv_obj_remove_style(ap_confirm_arc, NULL, LV_PART_KNOB);

    lv_obj_t *apc_t = lv_label_create(ovl_ap_confirm);
    lv_label_set_text(apc_t, "TURN RIGHT\nTO CONFIRM");
    lv_obj_set_style_text_align(apc_t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(apc_t, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(apc_t, COL_CYAN, 0);
    lv_obj_align(apc_t, LV_ALIGN_CENTER, 0, -6);

    ap_confirm_lbl_sec = lv_label_create(ovl_ap_confirm);
    lv_obj_set_style_text_font(ap_confirm_lbl_sec, &lv_font_montserrat_12, 0);
    lv_obj_align(ap_confirm_lbl_sec, LV_ALIGN_CENTER, 0, 34);

    // 5. Anchor Action Menu
    ovl_anc_menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_anc_menu, 240, 240);
    lv_obj_center(ovl_anc_menu);
    lv_obj_set_style_radius(ovl_anc_menu, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_anc_menu, COL_BG, 0);
    lv_obj_set_style_bg_opa(ovl_anc_menu, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ovl_anc_menu, 3, 0);
    lv_obj_set_style_border_color(ovl_anc_menu, COL_AMBER, 0);
    lv_obj_remove_flag(ovl_anc_menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; i++) {
        anc_menu_items[i] = lv_label_create(ovl_anc_menu);
        lv_obj_set_style_text_font(anc_menu_items[i], &lv_font_montserrat_14, 0);
        lv_obj_align(anc_menu_items[i], LV_ALIGN_CENTER, 0, -14 + i * 32);
    }

    // 6. Styled Boot & Pairing System Screen (Tick Ring + Curved Labels + Pill)
    scr_system = lv_obj_create(NULL);
    lv_obj_set_size(scr_system, 240, 240);
    lv_obj_set_style_bg_color(scr_system, COL_BG, 0);
    lv_obj_remove_flag(scr_system, LV_OBJ_FLAG_SCROLLABLE);

    build_radial_tick_ring(scr_system, sys_tick_lines, sys_tick_pts);
    update_radial_tick_ring(sys_tick_lines, sys_tick_pts, 0.0f, true, true);

    sys_canvas_top = create_curved_canvas(scr_system, LV_ALIGN_TOP_MID, &sys_buf_top);
    sys_canvas_bot = create_curved_canvas(scr_system, LV_ALIGN_BOTTOM_MID, &sys_buf_bot);

    sys_arc_prog = lv_arc_create(scr_system);
    lv_obj_set_size(sys_arc_prog, 168, 168);
    lv_obj_center(sys_arc_prog);
    lv_arc_set_rotation(sys_arc_prog, 270);
    lv_arc_set_bg_angles(sys_arc_prog, 0, 360);
    lv_obj_remove_style(sys_arc_prog, NULL, LV_PART_KNOB);

    sys_pill_box = lv_obj_create(scr_system);
    lv_obj_set_size(sys_pill_box, 172, 28);
    lv_obj_align(sys_pill_box, LV_ALIGN_CENTER, 0, -24);
    lv_obj_set_style_radius(sys_pill_box, 14, 0);
    lv_obj_set_style_bg_color(sys_pill_box, COL_CARD, 0);
    lv_obj_set_style_border_width(sys_pill_box, 1, 0);
    lv_obj_set_style_border_color(sys_pill_box, COL_AMBER, 0);
    lv_obj_remove_flag(sys_pill_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sys_pill_box, LV_OBJ_FLAG_HIDDEN);

    // Connecting steps: a drawn status mark (circle) plus a label per step, no glyph fonts needed.
    static const char *step_names[3] = { "WI-FI", "SERVER", "LINK" };
    for (int i = 0; i < 3; i++) {
        sys_step_dot[i] = lv_obj_create(scr_system);
        lv_obj_set_size(sys_step_dot[i], 16, 16);
        lv_obj_set_style_radius(sys_step_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(sys_step_dot[i], 0, 0);
        lv_obj_remove_flag(sys_step_dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(sys_step_dot[i], LV_ALIGN_CENTER, -44, -24 + i * 26);

        sys_step_chk[i] = lv_label_create(sys_step_dot[i]);
        lv_label_set_text(sys_step_chk[i], LV_SYMBOL_OK);          // exists in the built-in Montserrat symbol range
        lv_obj_set_style_text_font(sys_step_chk[i], &lv_font_montserrat_12, 0);
        lv_obj_center(sys_step_chk[i]);

        sys_step_lbl[i] = lv_label_create(scr_system);
        lv_label_set_text(sys_step_lbl[i], step_names[i]);
        lv_obj_set_style_text_font(sys_step_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_align(sys_step_lbl[i], LV_ALIGN_CENTER, 14, -24 + i * 26);

        lv_obj_add_flag(sys_step_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_step_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    sys_lbl_title = lv_label_create(scr_system);
    lv_obj_set_style_text_font(sys_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_align(sys_lbl_title, LV_ALIGN_CENTER, 0, -24);

    sys_lbl_line1 = lv_label_create(scr_system);
    lv_obj_set_style_text_align(sys_lbl_line1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(sys_lbl_line1, &lv_font_montserrat_12, 0);
    lv_obj_align(sys_lbl_line1, LV_ALIGN_CENTER, 0, 6);

    sys_lbl_line2 = lv_label_create(scr_system);
    lv_obj_set_style_text_font(sys_lbl_line2, &lv_font_montserrat_12, 0);
    lv_obj_align(sys_lbl_line2, LV_ALIGN_CENTER, 0, 34);

    // Boot splash on top of everything else on this screen, hidden unless the system screen is BOOT.
    sys_splash = lv_image_create(scr_system);
    lv_image_set_src(sys_splash, &marine_splash_img);
    lv_obj_set_pos(sys_splash, 0, 0); // the 1 px shift is baked into splash_img.c (tools/png2lvgl.py --shift 1,1 --circle 118.5)
    lv_obj_remove_flag(sys_splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sys_splash, LV_OBJ_FLAG_HIDDEN);

    sys_splash_track = lv_obj_create(scr_system);
    lv_obj_remove_style_all(sys_splash_track);
    lv_obj_set_pos(sys_splash_track, SPLASH_BAR_X, SPLASH_BAR_Y);
    lv_obj_set_size(sys_splash_track, SPLASH_BAR_W, SPLASH_BAR_H);
    lv_obj_set_style_radius(sys_splash_track, SPLASH_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(sys_splash_track, lv_color_hex(0x012E68), 0);
    lv_obj_set_style_bg_opa(sys_splash_track, LV_OPA_COVER, 0);
    lv_obj_add_flag(sys_splash_track, LV_OBJ_FLAG_HIDDEN);

    sys_splash_fill = lv_obj_create(scr_system);
    lv_obj_remove_style_all(sys_splash_fill);
    lv_obj_set_pos(sys_splash_fill, SPLASH_BAR_X, SPLASH_BAR_Y);
    lv_obj_set_size(sys_splash_fill, 0, SPLASH_BAR_H);
    lv_obj_set_style_radius(sys_splash_fill, SPLASH_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(sys_splash_fill, lv_color_hex(0xFEBF11), 0);
    lv_obj_set_style_bg_opa(sys_splash_fill, LV_OPA_COVER, 0);
    lv_obj_add_flag(sys_splash_fill, LV_OBJ_FLAG_HIDDEN);

    sys_splash_pill = lv_obj_create(scr_system);
    lv_obj_remove_style_all(sys_splash_pill);
    lv_obj_set_size(sys_splash_pill, SPLASH_PILL_W, SPLASH_PILL_H);
    lv_obj_set_pos(sys_splash_pill, (240 - SPLASH_PILL_W) / 2, SPLASH_PILL_Y);
    lv_obj_set_style_radius(sys_splash_pill, SPLASH_PILL_H / 2, 0);
    lv_obj_set_style_bg_color(sys_splash_pill, lv_color_hex(0x012E68), 0);
    lv_obj_set_style_bg_opa(sys_splash_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sys_splash_pill, 1, 0);
    lv_obj_set_style_border_color(sys_splash_pill, lv_color_hex(0x2F5E9B), 0);
    lv_obj_remove_flag(sys_splash_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sys_splash_pill, LV_OBJ_FLAG_HIDDEN);

    sys_splash_phase = lv_label_create(sys_splash_pill);
    lv_obj_set_style_text_font(sys_splash_phase, &lv_font_montserrat_12, 0);
    lv_label_set_text(sys_splash_phase, "");
    lv_obj_center(sys_splash_phase);
}

// Show or hide the boot splash and set the live progress bar (0..100).
static void sys_splash_show(bool show, uint8_t pct) {
    if (!show) {
        lv_obj_add_flag(sys_splash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_splash_track, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_splash_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_splash_pill, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (pct > 100) pct = 100;
    lv_obj_remove_flag(sys_splash, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sys_splash_track, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(sys_splash_fill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(sys_splash_fill, (SPLASH_BAR_W * pct) / 100);
    lv_obj_remove_flag(sys_splash_pill, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(sys_splash_phase, snap.boot_phase);
    lv_obj_set_style_text_color(sys_splash_phase, snap.night_palette ? COL_NIGHT_RED : lv_color_hex(0xE8F0FF), 0);
    lv_obj_center(sys_splash_phase);
    lv_obj_set_style_bg_color(sys_splash_fill, snap.night_palette ? COL_NIGHT_RED : lv_color_hex(0xFEBF11), 0);
}

// --- Transient previews and prompts are dropped when a mode goes stale or the mode changes ---
static void cancel_transients(void) {
    if (ap_confirm_open) {
        ap_confirm_open = false;
        lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
    }
    if (anchor_menu_open) {
        anchor_menu_open = false;
        lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
    }
    ap_pending_active = false;
    ap_pending_sent = false;
    dimmer_pending_active = false;
    dimmer_pending_sent = false;
    dimmer_wake_active = false;
    anchor_preview_active = false;
}

// --- Stale overlay, evaluated every tick and on every snapshot (a silent link never calls apply_snapshot) ---
static void update_stale_overlay(void) {
    stale_state_t st = (snap.system_screen == MARINE_SCREEN_OPERATIONAL) ? current_stale() : STALE_NONE;
    const char *reason = (st == STALE_ESSENTIAL) ? essential_stale_reason() : NULL;
    uint32_t age_s = 0;
    if (st == STALE_LINK) {
        age_s = (snap.now_ms >= snap.last_data_ms) ? ((snap.now_ms - snap.last_data_ms) / 1000U) : 0U;
    }

    uint32_t sig = display_stale_bits()
                 | ((uint32_t)st << 8)
                 | ((uint32_t)active_mode << 12)
                 | (mode_ring_open ? (1u << 16) : 0u)
                 | (is_full_alarm_blocking() ? (1u << 17) : 0u);
    bool changed = (sig != last_stale_sig) || (reason != last_stale_reason);
    bool age_changed = (st == STALE_LINK) && (age_s != last_stale_age_s);
    if (!changed && !age_changed) return;
    last_stale_sig = sig;
    last_stale_reason = reason;
    last_stale_age_s = age_s;

    if (changed && st != STALE_NONE) cancel_transients();

    // The overlay never covers the mode ring (the user can always leave) or the full-screen alarm.
    bool show = (st != STALE_NONE) && !mode_ring_open && !is_full_alarm_blocking();
    if (show) {
        if (st == STALE_LINK) {
            char b[32];
            snprintf(b, sizeof(b), "LAST DATA %lu s AGO", (unsigned long)age_s);
            lv_label_set_text(stale_lbl_title, "LINK LOST");
            lv_label_set_text(stale_lbl_age, b);
            lv_label_set_text(stale_lbl_sub, "CONTROLS OFF");
        } else {
            lv_label_set_text(stale_lbl_title, reason);
            lv_label_set_text(stale_lbl_age, "VALUE TOO OLD");
            lv_label_set_text(stale_lbl_sub, "2x PRESS: MODES");
        }
        lv_obj_remove_flag(ovl_stale, LV_OBJ_FLAG_HIDDEN);
        // keep the persistent acknowledged-alarm ring and badge above the stale overlay
        lv_obj_move_foreground(ovl_alarm_ack_ring);
        lv_obj_move_foreground(ovl_alarm_ack_badge);
    } else {
        lv_obj_add_flag(ovl_stale, LV_OBJ_FLAG_HIDDEN);
    }

    // Re-render the active mode so masked values follow the stale state.
    if (changed && snap.system_screen == MARINE_SCREEN_OPERATIONAL && mode_registry[active_mode].refresh) {
        mode_registry[active_mode].refresh();
    }
}

// --- Acknowledged-alarm ring: slow opacity pulse (switchable), steady when the pulse switch is off ---
static bool ack_ring_animating = false;

static void ack_ring_anim_cb(void *var, int32_t v) {
    int32_t d = (v < 900) ? v : (1800 - v);                 // triangle wave 0..900..0
    lv_opa_t opa = (lv_opa_t)(90 + (d * 165) / 900);
    lv_obj_set_style_border_opa((lv_obj_t *)var, opa, 0);
}

static void set_ack_indicator(bool on) {
    if (on) {
        lv_obj_remove_flag(ovl_alarm_ack_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ovl_alarm_ack_badge, LV_OBJ_FLAG_HIDDEN);
        if (snap.fx_ring_pulse_enabled && !ack_ring_animating) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, ovl_alarm_ack_ring);
            lv_anim_set_exec_cb(&a, ack_ring_anim_cb);
            lv_anim_set_values(&a, 0, 1800);
            lv_anim_set_duration(&a, 1800);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_start(&a);
            ack_ring_animating = true;
        } else if (!snap.fx_ring_pulse_enabled && ack_ring_animating) {
            lv_anim_delete(ovl_alarm_ack_ring, ack_ring_anim_cb);
            ack_ring_animating = false;
            lv_obj_set_style_border_opa(ovl_alarm_ack_ring, LV_OPA_COVER, 0);
        }
    } else {
        lv_obj_add_flag(ovl_alarm_ack_ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ovl_alarm_ack_badge, LV_OBJ_FLAG_HIDDEN);
        if (ack_ring_animating) {
            lv_anim_delete(ovl_alarm_ack_ring, ack_ring_anim_cb);
            ack_ring_animating = false;
        }
        lv_obj_set_style_border_opa(ovl_alarm_ack_ring, LV_OPA_COVER, 0);
    }
}

// --- Connecting steps: drawn marks. done = green disc with check, active = cyan ring, pending = grey ring ---
static void sys_set_steps(bool show, uint8_t step) {
    for (int i = 0; i < 3; i++) {
        if (!show) {
            lv_obj_add_flag(sys_step_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(sys_step_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(sys_step_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sys_step_lbl[i], LV_OBJ_FLAG_HIDDEN);
        if (i < step) {
            lv_obj_set_style_bg_color(sys_step_dot[i], snap.night_palette ? COL_NIGHT_RED : COL_GREEN, 0);
            lv_obj_set_style_bg_opa(sys_step_dot[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sys_step_dot[i], 0, 0);
            lv_obj_remove_flag(sys_step_chk[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(sys_step_chk[i], COL_BG, 0);
            lv_obj_set_style_text_color(sys_step_lbl[i], c_text(), 0);
        } else if (i == step) {
            lv_obj_set_style_bg_color(sys_step_dot[i], c_primary(), 0);
            lv_obj_set_style_bg_opa(sys_step_dot[i], LV_OPA_30, 0);
            lv_obj_set_style_border_width(sys_step_dot[i], 3, 0);
            lv_obj_set_style_border_color(sys_step_dot[i], c_primary(), 0);
            lv_obj_add_flag(sys_step_chk[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(sys_step_lbl[i], c_primary(), 0);
        } else {
            lv_obj_set_style_bg_opa(sys_step_dot[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(sys_step_dot[i], 2, 0);
            lv_obj_set_style_border_color(sys_step_dot[i], c_muted(), 0);
            lv_obj_add_flag(sys_step_chk[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(sys_step_lbl[i], c_muted(), 0);
        }
    }
}

// --- Semantic Event Dispatch ---
static void dispatch_semantic(marine_semantic_input_t ev, int32_t val) {
    bool full_alarm = is_full_alarm_blocking();
    if (!note_user_activity(full_alarm && ev == MARINE_INPUT_SINGLE_PRESS)) return;

    if (full_alarm) {
        if (ev == MARINE_INPUT_SINGLE_PRESS) {
            emit_action(active_mode, MARINE_ACT_ALARM_ACK, (int32_t)snap.active_alarm, 0);
        }
        return;
    }

    if (snap.system_screen != MARINE_SCREEN_OPERATIONAL) {
        return;
    }

    // Stale does NOT block the mode ring (below): the user can always leave a stale mode. Only the mode's own
    // controls are gated further down. HID mode is never stale (it needs the USB host, not the SignalK link).

    if (ev == MARINE_INPUT_DOUBLE_PRESS) {
        if (mode_ring_open) close_mode_ring(false);
        else open_mode_ring();
        return;
    }

    if (mode_ring_open) {
        if (ev == MARINE_INPUT_TURN) {
            ring_opened_ms = snap.now_ms;
            for (int i = 1; i <= 4; i++) {
                uint8_t next = (uint8_t)((ring_selected_slot + val * i + 16) % 4);
                if (ring_slot_modes[next] == MARINE_MODE_USB_HID && !snap.usb_host_enumerated) continue;
                ring_selected_slot = next;
                break;
            }
            refresh_mode_ring();
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            close_mode_ring(true);
        }
        return;
    }

    if (current_stale() != STALE_NONE) {
        return; // the active mode's own controls are off while its data is stale
    }

    if (ap_confirm_open) {
        if (ev == MARINE_INPUT_TURN && val > 0) {
            ap_confirm_open = false;
            lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
            emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_ENGAGE, 1, 0);
        } else if (ev == MARINE_INPUT_SINGLE_PRESS || ev == MARINE_INPUT_LONG_PRESS) {
            ap_confirm_open = false;
            lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (anchor_menu_open) {
        uint8_t count = snap.anchor_armed ? 2 : 1;
        if (ev == MARINE_INPUT_TURN) {
            anchor_menu_opened_ms = snap.now_ms;
            anchor_menu_idx = (uint8_t)((anchor_menu_idx + val + 4) % count);
            lv_obj_set_style_text_color(anc_menu_items[0], anchor_menu_idx == 0 ? c_amber() : c_text(), 0);
            lv_obj_set_style_text_color(anc_menu_items[1], anchor_menu_idx == 1 ? c_amber() : c_text(), 0);
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            anchor_menu_open = false;
            lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
            if (!snap.anchor_armed) emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_ARM_HERE, 1, 0);
            else if (anchor_menu_idx == 0) emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_MOVE_DROP, 1, 0);
            else emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_DISARM, 1, 0);
        }
        return;
    }

    if (mode_registry[active_mode].on_input) {
        mode_registry[active_mode].on_input(ev, val);
    }
}

// --- Public API ---
void marine_ui_init(marine_ui_action_cb_t action_cb, void *user_ctx) {
    s_action_cb = action_cb;
    s_action_ctx = user_ctx;
    // Clean zero/false defaults; firmware owns initial SignalK state
    memset(&snap, 0, sizeof(snap));
    snap.alarm_ack_return_with_badge = true;

    for (int i = 0; i < MARINE_MODE_COUNT; i++) {
        scr_modes[i] = mode_registry[i].build_screen();
    }
    build_overlays_and_system();
    lv_screen_load(scr_modes[active_mode]);
    mode_registry[active_mode].refresh();
}

void marine_ui_load_demo_snapshot(marine_ui_snapshot_t *out_snap) {
    if (!out_snap) return;
    memset(out_snap, 0, sizeof(*out_snap));
    out_snap->signalk_connected = true;
    out_snap->fx_glass_enabled = true;
    out_snap->fx_glow_enabled = true;
    out_snap->fx_ring_pulse_enabled = true;
    out_snap->alarm_ack_return_with_badge = true;
    out_snap->ap_engaged = true;
    out_snap->heading_deg = 284.0f;
    out_snap->cts_deg = 285.0f;
    out_snap->rudder_deg = 3.5f;
    out_snap->depth_m = 6.4f;
    out_snap->voltage_v = 12.8f;
    out_snap->gps_3d_fix = true;
    out_snap->anchor_armed = true;
    out_snap->anchor_radius_m = 35;
    out_snap->anchor_dist_m = 14.0f;
    out_snap->anchor_bearing_deg = 85.0f;
    out_snap->dimmer_group_count = 3;
    strcpy(out_snap->dimmer_groups[0].name, "SALOON");
    out_snap->dimmer_groups[0].on = true;
    out_snap->dimmer_groups[0].level_pct = 65;
    strcpy(out_snap->dimmer_groups[1].name, "COCKPIT");
    out_snap->dimmer_groups[1].on = true;
    out_snap->dimmer_groups[1].level_pct = 40;
    strcpy(out_snap->dimmer_groups[2].name, "CABIN");
    out_snap->dimmer_groups[2].on = false;
    out_snap->dimmer_groups[2].level_pct = 0;
    out_snap->usb_host_enumerated = true;
    strcpy(out_snap->device_name, "HELM-DIAL-04");
    strcpy(out_snap->setup_ap_ssid, "HelmDial-Setup-04");
}

void marine_ui_feed_encoder_step(int8_t step_delta) {
    if (step_delta == 0) return;
    dispatch_semantic(MARINE_INPUT_TURN, step_delta);
}

void marine_ui_feed_button_edge(bool pressed) {
    if (pressed) {
        btn_is_down = true;
        btn_down_ms = snap.now_ms;
        btn_long_fired = false;
        if (snap.system_screen == MARINE_SCREEN_RESET_SETUP) {
            reset_holding = true;
            reset_hold_start_ms = snap.now_ms;
        }
    } else {
        reset_holding = false;
        if (snap.system_screen == MARINE_SCREEN_RESET_SETUP) {
            lv_arc_set_value(sys_arc_prog, 0);
        }
        if (!btn_is_down) return;
        btn_is_down = false;
        if (!btn_long_fired) {
            btn_click_count++;
            btn_last_up_ms = snap.now_ms;
            if (btn_click_count >= 2) {
                btn_click_count = 0;
                dispatch_semantic(MARINE_INPUT_DOUBLE_PRESS, 0);
            }
        }
    }
}

void marine_ui_feed_touch(int16_t x, int16_t y, bool pressed) {
    if (!pressed) return;
    int32_t dx = (int32_t)x - 120;
    int32_t dy = (int32_t)y - 120;
    if ((dx * dx + dy * dy) < (50 * 50)) return;

    // Restricted to a single harmless role: step to next Dimmer group when in Dimmer mode
    if (snap.system_screen == MARINE_SCREEN_OPERATIONAL &&
        !is_full_alarm_blocking() &&
        !mode_ring_open &&
        current_stale() == STALE_NONE &&
        active_mode == MARINE_MODE_DIMMER &&
        !snap.dimmer_use_rgbw_variant) {
        if (!note_user_activity(false)) return;
        dimmer_on_input(MARINE_INPUT_SINGLE_PRESS, 0);
    }
}

void marine_ui_apply_snapshot(const marine_ui_snapshot_t *snapshot) {
    if (!snapshot) return;
    uint32_t prev_now_ms = snap.now_ms;
    snap = *snapshot;
    if (snap.now_ms < prev_now_ms) snap.now_ms = prev_now_ms;   // keep the tick clock monotonic

    // Immediate backlight wake + fast LED flash when a new alarm arrives OR the alarm type changes
    // (a changed type needs a fresh acknowledge, so the screen must be awake for it).
    if (snap.active_alarm != MARINE_ALARM_NONE && snap.active_alarm != prev_alarm_state) {
        last_input_ms = snap.now_ms;
        if (current_backlight_pct != 100) {
            current_backlight_pct = 100;
            emit_action(active_mode, MARINE_ACT_POWER_SET_BACKLIGHT, 100, 0);
        }
        emit_action(active_mode, MARINE_ACT_ALARM_LED_PULSE, 1, 0);
    }
    if (snap.active_alarm == MARINE_ALARM_NONE && prev_alarm_state != MARINE_ALARM_NONE) {
        emit_action(active_mode, MARINE_ACT_ALARM_LED_PULSE, 0, 0);   // alarm cleared by the server
    }
    prev_alarm_state = snap.active_alarm;

    // Acknowledged but still active: slow red LED pulse (the firmware drives the LEDs from this action)
    bool ack_state = (snap.active_alarm != MARINE_ALARM_NONE) && snap.alarm_acknowledged && snap.alarm_ack_return_with_badge;
    if (ack_state != prev_ack_state) {
        prev_ack_state = ack_state;
        emit_action(active_mode, MARINE_ACT_ALARM_LED_SLOW_PULSE, ack_state ? 1 : 0, 0);
    }

    // 1. Alarm Takeover vs. Persistent Acknowledged Indicator
    if (is_full_alarm_blocking()) {
        lv_obj_remove_flag(ovl_alarm, LV_OBJ_FLAG_HIDDEN);
        set_ack_indicator(false);

        lv_color_t alarm_col = c_red();
        icon_recolor(alm_icon_anchor, ICON_ANCHOR, 48, alarm_col, &alm_icon_col);
        static uint32_t warn_col_cache = 0xFFFFFFFFu;
        icon_recolor(alm_icon_warn, ICON_WARN, 22, alarm_col, &warn_col_cache);

        bool is_drag = (snap.active_alarm == MARINE_ALARM_ANCHOR_DRAG);
        lv_label_set_text(alm_lbl_title, is_drag ? "DRAG ALARM!" : "GPS LOST!");
        lv_label_set_text(alm_lbl_ack, snap.alarm_acknowledged ? "ACKNOWLEDGED" : "PRESS TO ACKNOWLEDGE");
        lv_obj_set_style_text_color(alm_lbl_ack, snap.alarm_acknowledged ? c_amber() : c_text(), 0);
        if (is_drag) lv_obj_remove_flag(alm_boat_dot, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(alm_boat_dot, LV_OBJ_FLAG_HIDDEN);

        char top_b[48], bot_b[48];
        if (is_drag) snprintf(top_b, sizeof(top_b), "ANCHORED • %dm / %um", (int)roundf(snap.anchor_dist_m), snap.anchor_radius_m);
        else snprintf(top_b, sizeof(top_b), "ANCHOR WATCH • FIX LOST");
        snprintf(bot_b, sizeof(bot_b), "%.1fm • %.1fV • %s", snap.depth_m, snap.voltage_v, is_drag ? "3D FIX" : "NO GPS FIX");
        update_curved_canvas(alm_canvas_top, alm_cache_top, top_b, false, c_red());
        update_curved_canvas(alm_canvas_bot, alm_cache_bot, bot_b, true, is_drag ? c_muted() : c_red());
        update_stale_overlay();   // hides the stale overlay behind the full-screen alarm
        return;
    }
    lv_obj_add_flag(ovl_alarm, LV_OBJ_FLAG_HIDDEN);

    if (ack_state) {
        lv_label_set_text(ovl_alarm_ack_lbl, snap.active_alarm == MARINE_ALARM_ANCHOR_DRAG ? "DRAG ALARM" : "GPS LOST");
        set_ack_indicator(true);
    } else {
        set_ack_indicator(false);
    }

    // 2. Styled Boot / Provisioning / Pairing System Screens
    if (snap.system_screen != MARINE_SCREEN_OPERATIONAL) {
        if (lv_screen_active() != scr_system) lv_screen_load(scr_system);
        lv_obj_add_flag(sys_arc_prog, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_pill_box, LV_OBJ_FLAG_HIDDEN);
        sys_set_steps(false, 0);
        lv_obj_add_flag(ovl_stale, LV_OBJ_FLAG_HIDDEN);
        sys_splash_show(snap.system_screen == MARINE_SCREEN_BOOT, snap.boot_progress_pct);

        if (snap.system_screen == MARINE_SCREEN_BOOT) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "HELM DIAL", false, c_primary());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "INITIALIZING", true, c_muted());
            lv_obj_remove_flag(sys_arc_prog, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_value(sys_arc_prog, snap.boot_progress_pct);
            lv_label_set_text(sys_lbl_title, "HELM DIAL");
            lv_label_set_text(sys_lbl_line1, "STARTING");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_WIFI_PORTAL) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "JOIN WI-FI", false, c_amber());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "SETUP ONLY", true, c_muted());
            lv_obj_remove_flag(sys_pill_box, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sys_lbl_title, snap.setup_ap_ssid[0] ? snap.setup_ap_ssid : "HelmDial-Setup");
            lv_label_set_text(sys_lbl_line1, "THEN OPEN THE");
            lv_label_set_text(sys_lbl_line2, "SETUP PAGE");
        } else if (snap.system_screen == MARINE_SCREEN_CONNECTING) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "CONNECTING", false, c_primary());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "PLEASE WAIT", true, c_muted());
            sys_set_steps(true, snap.connect_step);   // drawn marks, no glyph fonts
            lv_label_set_text(sys_lbl_title, "");
            lv_label_set_text(sys_lbl_line1, "");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_PAIRING) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "PAIRING", false, c_amber());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "WAITING APPROVAL", true, c_muted());
            lv_label_set_text(sys_lbl_title, "APPROVE KNOB");
            lv_label_set_text(sys_lbl_line1, "IN SIGNALK ADMIN UI");
            lv_label_set_text(sys_lbl_line2, snap.device_name);
        } else if (snap.system_screen == MARINE_SCREEN_CANNOT_CONNECT) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "CONNECTION ERROR", false, c_red());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "RETRYING", true, c_muted());
            lv_label_set_text(sys_lbl_title, "NO SERVER");
            lv_label_set_text(sys_lbl_line1, "WI-FI OK");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_RESET_SETUP) {
            update_curved_canvas(sys_canvas_top, sys_cache_top, "FACTORY RESET", false, c_red());
            update_curved_canvas(sys_canvas_bot, sys_cache_bot, "RELEASE TO CANCEL", true, c_muted());
            lv_obj_remove_flag(sys_arc_prog, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sys_lbl_title, "HOLD 5 SECONDS");
            lv_label_set_text(sys_lbl_line1, "CLEARS WI-FI");
            lv_label_set_text(sys_lbl_line2, "AND PAIRING");
        }
        return;
    }

    if (lv_screen_active() != scr_modes[active_mode]) {
        lv_screen_load(scr_modes[active_mode]);
    }

    // 3. Stale link / stale value overlay (also re-evaluated every tick)
    update_stale_overlay();

    if (mode_registry[active_mode].refresh) {
        mode_registry[active_mode].refresh();
    }
    if (mode_ring_open) {
        refresh_mode_ring_center_preview();
    }
}

void marine_ui_tick(uint32_t now_ms) {
    snap.now_ms = now_ms;

    // 1. Button classifier timers (300 ms single-press window, 750 ms long-press)
    if (btn_is_down && !btn_long_fired && (now_ms - btn_down_ms >= 750)) {
        btn_long_fired = true;
        btn_click_count = 0;
        dispatch_semantic(MARINE_INPUT_LONG_PRESS, 0);
    }
    if (!btn_is_down && btn_click_count == 1 && (now_ms - btn_last_up_ms >= 300)) {
        btn_click_count = 0;
        dispatch_semantic(MARINE_INPUT_SINGLE_PRESS, 0);
    }

    // 2. Autopilot 400 ms pause commit; keep pending preview until snapshot confirms or timeout expires
    if (ap_pending_active) {
        if (!ap_pending_sent && now_ms >= ap_pending_deadline_ms) {
            ap_pending_sent = true;
            emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_SET_CTS, ap_pending_cts, 0);
        }
        int32_t srv_cts = (int32_t)roundf(wrap_deg(snap.cts_deg));
        if (ap_pending_sent && (srv_cts == ap_pending_cts || now_ms >= (ap_pending_deadline_ms + MARINE_PENDING_CONFIRM_TIMEOUT))) {
            ap_pending_active = false;
            ap_pending_sent = false;
            ap_refresh();
        }
    }

    // 3a. Switch-on latch: release when the server shows the group on, or after the confirm timeout
    if (dimmer_wake_active) {
        bool on_now = dimmer_wake_group < snap.dimmer_group_count && snap.dimmer_groups[dimmer_wake_group].on;
        if (on_now || now_ms >= dimmer_wake_deadline_ms) dimmer_wake_active = false;
    }

    // 3. Dimmer buffered pending level/preset commit
    if (dimmer_pending_active) {
        if (!dimmer_pending_sent && now_ms >= dimmer_pending_deadline_ms) {
            dimmer_pending_sent = true;
            if (snap.dimmer_use_rgbw_variant) {
                emit_action(MARINE_MODE_DIMMER, MARINE_ACT_DIMMER_SET_PRESET, dimmer_pending_preset, 0);
            } else {
                emit_action(MARINE_MODE_DIMMER, MARINE_ACT_DIMMER_SET_LEVEL, dimmer_pending_level, dimmer_selected_group);
            }
        }
        bool confirmed = snap.dimmer_use_rgbw_variant
            ? ((snap.rgbw_preset_idx % MARINE_RGBW_PRESET_COUNT) == dimmer_pending_preset)
            : (snap.dimmer_groups[dimmer_selected_group].level_pct == dimmer_pending_level);
        if (dimmer_pending_sent && (confirmed || now_ms >= (dimmer_pending_deadline_ms + MARINE_PENDING_CONFIRM_TIMEOUT))) {
            dimmer_pending_active = false;
            dimmer_pending_sent = false;
            dimmer_refresh();
        }
    }

    // 4. Prompt & Mode Ring 5 s idle timeouts
    if (ap_confirm_open) {
        if (now_ms >= ap_confirm_deadline_ms) {
            ap_confirm_open = false;
            lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        } else {
            uint32_t rem = ap_confirm_deadline_ms - now_ms;
            lv_arc_set_value(ap_confirm_arc, (int32_t)rem);
            char sb[24];
            snprintf(sb, sizeof(sb), "%lu s", (unsigned long)((rem + 999) / 1000));
            lv_label_set_text(ap_confirm_lbl_sec, sb);
        }
    }
    if (mode_ring_open && (now_ms - ring_opened_ms >= 5000)) {
        close_mode_ring(false);
    }
    if (anchor_menu_open && (now_ms - anchor_menu_opened_ms >= 5000)) {
        anchor_menu_open = false;
        lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
    }

    // 5. Reset setup hold arc (fires only at full MARINE_FACTORY_RESET_HOLD_MS = 5000 ms)
    if (reset_holding && snap.system_screen == MARINE_SCREEN_RESET_SETUP) {
        uint32_t elapsed = now_ms - reset_hold_start_ms;
        uint32_t pct = (elapsed * 100U) / MARINE_FACTORY_RESET_HOLD_MS;
        if (pct > 100U) pct = 100U;
        lv_arc_set_value(sys_arc_prog, (int32_t)pct);
        if (elapsed >= MARINE_FACTORY_RESET_HOLD_MS) {
            reset_holding = false;
            emit_action(active_mode, MARINE_ACT_SYS_FACTORY_RESET, 1, 0);
        }
    }

    // 5b. Staleness is time-based: re-evaluate every tick, a silent link never calls marine_ui_apply_snapshot()
    update_stale_overlay();

    // 6. Idle backlight dimming (30 s -> 10 %, 10 min -> 0 % unless AP engaged or Alarm active)
    uint32_t idle_ms = now_ms - last_input_ms;
    bool keep_awake = snap.ap_engaged || (snap.active_alarm != MARINE_ALARM_NONE);
    uint8_t target_bl = 100;
    if (idle_ms > MARINE_IDLE_OFF_TIME_MS && !keep_awake) target_bl = 0;
    else if (idle_ms > MARINE_IDLE_DIM_TIME_MS) target_bl = MARINE_IDLE_DIM_LEVEL_PCT;

    if (target_bl != current_backlight_pct) {
        current_backlight_pct = target_bl;
        emit_action(active_mode, MARINE_ACT_POWER_SET_BACKLIGHT, target_bl, 0);
    }
}

marine_ui_mode_t marine_ui_get_active_mode(void) {
    return active_mode;
}
