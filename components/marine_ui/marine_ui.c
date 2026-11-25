#include "marine_ui.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
#include "knob_logic.h"
#include "knob_fonts.h"

static void dfx_create(void);   // automatic demo overlay (#10), at the end of the file

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
// The three above are the defaults. The firmware sets the real values from its settings with marine_ui_set_power().
static uint32_t idle_dim_ms = MARINE_IDLE_DIM_TIME_MS;
static uint32_t idle_off_ms = MARINE_IDLE_OFF_TIME_MS;   // 0 = never fully off
static uint8_t idle_dim_pct = (uint8_t)MARINE_IDLE_DIM_LEVEL_PCT;
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

// Mode ring slots in ring order, clockwise (owner, 2026-11-24): motor, autopilot, anchor, lights, music, USB, setup. The
// visible ones are spaced evenly with AUTO at 12 o'clock, so MOTOR sits just left of it. A switched-off mode is left out and
// the order of the others stays (ui-design section 2).
static const marine_ui_mode_t ring_slot_modes[MARINE_MODE_COUNT] = {
    MARINE_MODE_MOTOR,
    MARINE_MODE_AUTOPILOT,       // RING_TOP_SLOT
    MARINE_MODE_ANCHOR_WATCH,
    MARINE_MODE_DIMMER,
    MARINE_MODE_MUSIC,
    MARINE_MODE_USB_HID,
    MARINE_MODE_SETTINGS
};
#define RING_TOP_SLOT 1   // drawn at 12 o'clock; when hidden, the next visible slot after it is

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
static int ap_confirm_dir = 0;            // 0 engage (turn right), -1 tack port (turn left), +1 tack starboard (turn right)
// Pilot dialog (#7): long press while engaged. STANDBY first and focused, the other modes, the tacks, CANCEL.
static bool pilot_open = false;
static uint8_t pilot_idx = 0, pilot_count = 0;
static knob_pilot_line_t pilot_lines[7];
static uint32_t pilot_opened_ms = 0;

static bool anchor_preview_active = false;
static uint16_t anchor_preview_radius = 35;
static bool anchor_menu_open = false;
static uint8_t anchor_menu_idx = 0;
static uint32_t anchor_menu_opened_ms = 0;
typedef struct { const char *text; marine_ui_action_type_t act; bool cancel; bool danger; } anc_item_t;
static anc_item_t anc_items[3];
static uint8_t anc_menu_count = 0;
// After a choice the screen shows a pending screen until the firmware reports what became of it
typedef enum { ANC_PEND_NONE = 0, ANC_PEND_WAIT, ANC_PEND_REFUSED } anc_pend_t;
static anc_pend_t anc_pend = ANC_PEND_NONE;
static marine_ui_action_type_t anc_pend_act = MARINE_ACT_ANCHOR_DROP;
static int8_t anc_pend_expect = -1;     // 1 = the anchor should read dropped, 0 = raised, -1 = no state change expected
static bool anc_pend_ok = false;        // the server accepted the call
static uint16_t anc_seen_seq = 0;
static uint32_t anc_pend_start_ms = 0, anc_pend_ok_ms = 0, anc_pend_until_ms = 0;

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
static lv_obj_t *ovl_anc_res;
static lv_obj_t *ovl_demo_tag;     // DEMO at the bottom edge while in demo mode
static lv_obj_t *ovl_ctl_ring;     // CONTROL ON notice: green rim
static lv_obj_t *ovl_ctl_pill;     // CONTROL ON notice: pill with title and source line
static lv_obj_t *ctl_lbl_title;
static lv_obj_t *ctl_lbl_sub;
static bool ctl_notice_on;
static uint32_t ctl_notice_until_ms;
static uint16_t ctl_last_seq;
#define CTL_NOTICE_MS 3000

// Touch. Pushing the knob presses the glass and reads as a touch near the centre about 230 ms before the button switch closes,
// so touches within this radius never act (SPEC section 12). They still wake a dark screen like any touch.
#define TOUCH_DEADZONE_PX 50
#define TOUCH_WAKE_SWALLOW_MS 500   // a button press starting this soon after a touch woke the screen is swallowed too
static bool touch_is_down;
static bool touch_woke;             // the last touch-down woke a dark screen
static uint32_t touch_wake_ms;
static bool btn_swallow;            // the current button press only belongs to that wake, it does nothing

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

// Anchor widgets
static lv_obj_t *anc_ring;
static lv_obj_t *anc_boat_dot;
static lv_obj_t *anc_lbl_center;
static lv_obj_t *anc_ring_half, *anc_ring_prev, *anc_icon, *anc_trail[3];
static lv_obj_t *anc_lbl_radius, *anc_lbl_dist, *anc_lbl_hint, *anc_lbl_big;
static uint8_t *anc_icon_buf = NULL;
static uint32_t anc_icon_col = 0xFFFFFFFFu;
static lv_obj_t *anc_lbl_state;   // top line, straight: RAISED, DROPPED, ANCHOR when stale
static lv_obj_t *anc_lbl_telem;   // bottom line, straight: depth, voltage, GPS fix

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
static lv_obj_t *ring_slot_boxes[MARINE_MODE_COUNT];
static lv_obj_t *ring_slot_labels[MARINE_MODE_COUNT];
static lv_obj_t *ring_slot_icons[MARINE_MODE_COUNT];
static lv_obj_t *ring_center_cut;
static lv_obj_t *ring_center_top;
static lv_obj_t *ring_center_mid;
static lv_obj_t *ring_center_bot;

// Alarm widgets
static lv_obj_t *alm_lbl_title;   // above the icon: DRAG ALARM or GPS LOST
static lv_obj_t *alm_icon;        // one big symbol in the middle, pulsing (knob_icon_96)
static bool alm_pulsing = false;
static lv_obj_t *alm_lbl_dist;    // below the icon: distance and radius, or NO GPS FIX
static lv_obj_t *alm_lbl_ack;


// Stale, Confirm, Anchor Menu & System Screen Widgets
static lv_obj_t *stale_lbl_age;
static lv_obj_t *ap_confirm_arc;
static lv_obj_t *ap_confirm_lbl_sec;
static lv_obj_t *ap_confirm_lbl_title;
static lv_obj_t *ovl_pilot, *pilot_title, *pilot_pill, *pilot_hint, *pilot_items[7];
static lv_obj_t *ap_lbl_hdr;      // HEADING, WIND - AWA, ROUTE - TO ...
static lv_obj_t *ap_wind_line;    // apparent wind from the centre (wind mode)
static lv_point_precise_t ap_wind_pts[2];
static lv_obj_t *anc_menu_items[3];
static lv_obj_t *anc_menu_title, *anc_menu_info, *anc_menu_pill, *anc_menu_hint;
static lv_obj_t *anc_res_arc, *anc_res_icon, *anc_res_t, *anc_res_s1, *anc_res_s2;
static uint8_t *anc_res_icon_buf = NULL;
static uint32_t anc_res_icon_col = 0xFFFFFFFFu;

static lv_obj_t *sys_tick_lines[72];
static lv_point_precise_t sys_tick_pts[72][2];
static lv_obj_t *sys_lbl_state;   // top line of the boot and setup screens, straight 20 px
static lv_obj_t *sys_lbl_bottom;  // bottom line of the setup screens (hidden on the splash, which has its own status)
static lv_obj_t *sys_pill_box;
static lv_obj_t *sys_step_dot[3];      // connecting steps: drawn status marks (no font glyphs needed)
static lv_obj_t *sys_step_chk[3];      // LV_SYMBOL_OK inside a finished step's dot
static lv_obj_t *sys_step_lbl[3];
static lv_obj_t *sys_lbl_title;
static lv_obj_t *sys_lbl_line1;
static lv_obj_t *sys_lbl_line2;
static lv_obj_t *sys_arc_prog;

// Boot splash: a full-screen RGB565 picture (splash_img.c, from assets/splash.png) covers the system screen while
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
// Pairing screen: large text in the middle.
static lv_obj_t *sys_pair_big;    // "PAIRING / WAITING FOR / APPROVAL", montserrat 28
static lv_obj_t *sys_pair_hint;   // where to approve
static lv_obj_t *sys_pair_name;   // the name the request shows in the SignalK admin UI
static lv_obj_t *sys_splash_pill;
static lv_obj_t *sys_splash_phase;

// Shared Scratch Glyph Canvas for Tangent Rotation (24x24 ARGB8888 in PSRAM)

// --- Forward Declarations ---
static lv_obj_t* anc_make_label(lv_obj_t *scr, const lv_font_t *font, int y_off);
static void ap_refresh_wind(void);
static void ap_refresh_route(void);
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

static lv_obj_t* build_settings_screen(void);
static void settings_on_input(marine_semantic_input_t ev, int32_t val);
static void settings_refresh(void);
static lv_obj_t* build_music_screen(void);
static void music_on_input(marine_semantic_input_t ev, int32_t val);
static void music_refresh(void);
static void music_tick(void);
static void music_touch(int16_t x, int16_t y);
static lv_obj_t* build_motor_screen(void);
static void motor_on_input(marine_semantic_input_t ev, int32_t val);
static void motor_refresh(void);
static bool settings_takes_double(void);
static void settings_tick(void);
static void settings_enter(void);

static const marine_mode_desc_t mode_registry[MARINE_MODE_COUNT] = {
    [MARINE_MODE_AUTOPILOT]    = { MARINE_MODE_AUTOPILOT,    "AUTO",   build_ap_screen,     ap_on_input,     ap_refresh },
    [MARINE_MODE_DIMMER]       = { MARINE_MODE_DIMMER,       "LIGHTS", build_dimmer_screen, dimmer_on_input, dimmer_refresh },
    [MARINE_MODE_ANCHOR_WATCH] = { MARINE_MODE_ANCHOR_WATCH, "ANCHOR", build_anchor_screen, anchor_on_input, anchor_refresh },
    [MARINE_MODE_USB_HID]      = { MARINE_MODE_USB_HID,      "USB",    build_hid_screen,    hid_on_input,    hid_refresh },
    [MARINE_MODE_SETTINGS]     = { MARINE_MODE_SETTINGS,     "SETUP",  build_settings_screen, settings_on_input, settings_refresh },
    [MARINE_MODE_MUSIC]       = { MARINE_MODE_MUSIC,       "MUSIC", build_music_screen, music_on_input, music_refresh },
    [MARINE_MODE_MOTOR]       = { MARINE_MODE_MOTOR,       "MOTOR", build_motor_screen, motor_on_input, motor_refresh }
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

// "T" or "M" after the heading degrees, "" when the firmware gives no reference
static const char *heading_ref_str(void) {
    return snap.heading_ref == 'T' ? "T" : (snap.heading_ref == 'M' ? "M" : "");
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
static uint8_t vol_sel_peek(void);

static const char *essential_stale_reason(void) {
    switch (active_mode) {
    case MARINE_MODE_MUSIC: {
        // The selected target's volume. Never received counts as missing, also for the event-driven master volume.
        uint8_t i = vol_sel_peek();
        const marine_vol_target_t *t = &snap.vol_targets[i];
        if (t->updated_ms == 0 || value_is_stale(t->updated_ms, t->timeout_ms)) return "NO VOLUME";
        return NULL;
    }
    case MARINE_MODE_AUTOPILOT:
        if (value_is_stale(snap.ap_state_updated_ms, snap.ap_state_timeout_ms)) return "NO AP STATE";
        if (value_is_stale(snap.heading_updated_ms, snap.heading_timeout_ms)) return "NO HEADING";
        if (snap.ap_engaged && snap.ap_state == KNOB_AP_WIND &&
            (snap.wind_updated_ms == 0 || value_is_stale(snap.wind_updated_ms, snap.wind_timeout_ms))) return "NO WIND";
        if (snap.ap_engaged && snap.ap_state == KNOB_AP_ROUTE &&
            (snap.route_updated_ms == 0 || value_is_stale(snap.route_updated_ms, snap.route_timeout_ms))) return "NO ROUTE";
        if (snap.ap_engaged && (snap.ap_state == KNOB_AP_WIND || snap.ap_state == KNOB_AP_ROUTE)) return NULL;
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
    // HID needs the USB host only, and the settings must work without a link (the setup access point is there to fix one).
    if (active_mode == MARINE_MODE_USB_HID || active_mode == MARINE_MODE_SETTINGS) return STALE_NONE;
    if (!snap.signalk_connected) return STALE_LINK;
    if (essential_stale_reason() != NULL) return STALE_ESSENTIAL;
    return STALE_NONE;
}

static void update_stale_overlay(void);
static void cancel_transients(void);
static void settings_edit_abort(void);

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

// Every line of text is straight (owner, 2026-11-23): a curved line drawn letter by letter along the rim was hard to read.
// Near the edge a straight line has little room: about 130 px at y 30 and 150 px at y 210, so the texts there are short.

// --- Drawn vector icons (no font glyphs needed): AUTO gauge, LIGHT bulb, ANCHOR, USB, WARNING triangle ---
typedef enum { ICON_AUTO = 0, ICON_LIGHT, ICON_ANCHOR, ICON_USB, ICON_GEAR, ICON_WARN, ICON_CROSS,
               ICON_MUSIC, ICON_PREV, ICON_NEXT, ICON_PLAY, ICON_PAUSE, ICON_PROP } icon_id_t;

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
    case ICON_GEAR:
        icon_circle(&layer, II(16), II(16), II(8), w, false, col);
        icon_circle(&layer, II(16), II(16), II(2.5f), 0, true, col);
        for (int k = 0; k < 8; k++) {
            float a = (float)k * (float)(M_PI / 4.0);
            icon_line(&layer, IP(16 + 9.5f * cosf(a)), IP(16 + 9.5f * sinf(a)), IP(16 + 13.5f * cosf(a)), IP(16 + 13.5f * sinf(a)),
                      w + 1, col);
        }
        break;
    case ICON_MUSIC:    // two beamed eighth notes
        icon_circle(&layer, II(9.5f), II(24), II(3.5f), 0, true, col);
        icon_circle(&layer, II(22.5f), II(21), II(3.5f), 0, true, col);
        icon_line(&layer, IP(12.5f), IP(24), IP(12.5f), IP(8), w, col);
        icon_line(&layer, IP(25.5f), IP(21), IP(25.5f), IP(5), w, col);
        icon_line(&layer, IP(12.5f), IP(8), IP(25.5f), IP(5), w + 2, col);
        icon_line(&layer, IP(12.5f), IP(12.5f), IP(25.5f), IP(9.5f), w + 1, col);
        break;
    case ICON_PROP:     // three-bladed propeller around a hub
        for (int k = 0; k < 3; k++) {
            float a = (float)(k * 120 - 90) * (float)(M_PI / 180.0);
            float b = a + 0.45f;
            icon_line(&layer, IP(16 + 4 * cosf(a)), IP(16 + 4 * sinf(a)), IP(16 + 13 * cosf(b)), IP(16 + 13 * sinf(b)), w + 3, col);
        }
        icon_circle(&layer, II(16), II(16), II(4), 0, true, col);
        break;
    case ICON_PREV:     // bar and triangle to the left
        icon_line(&layer, IP(8), IP(8), IP(8), IP(24), w, col);
        icon_line(&layer, IP(24), IP(8), IP(12), IP(16), w, col);
        icon_line(&layer, IP(12), IP(16), IP(24), IP(24), w, col);
        icon_line(&layer, IP(24), IP(24), IP(24), IP(8), w, col);
        break;
    case ICON_NEXT:
        icon_line(&layer, IP(24), IP(8), IP(24), IP(24), w, col);
        icon_line(&layer, IP(8), IP(8), IP(20), IP(16), w, col);
        icon_line(&layer, IP(20), IP(16), IP(8), IP(24), w, col);
        icon_line(&layer, IP(8), IP(24), IP(8), IP(8), w, col);
        break;
    case ICON_PLAY:
        icon_line(&layer, IP(11), IP(7), IP(25), IP(16), w, col);
        icon_line(&layer, IP(25), IP(16), IP(11), IP(25), w, col);
        icon_line(&layer, IP(11), IP(25), IP(11), IP(7), w, col);
        break;
    case ICON_PAUSE:
        icon_line(&layer, IP(11), IP(8), IP(11), IP(24), w + 2, col);
        icon_line(&layer, IP(21), IP(8), IP(21), IP(24), w + 2, col);
        break;
    case ICON_CROSS:
        icon_circle(&layer, II(16), II(16), II(13), w, false, col);
        icon_line(&layer, IP(10.5f), IP(10.5f), IP(21.5f), IP(21.5f), w, col);
        icon_line(&layer, IP(21.5f), IP(10.5f), IP(10.5f), IP(21.5f), w, col);
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
        snprintf(top_b, sizeof(top_b), "%03d°%s", (int)roundf(wrap_deg(snap.heading_deg)), heading_ref_str());
        snprintf(mid_b, sizeof(mid_b), "%s", snap.ap_engaged ? "AUTO" : "STANDBY");
        snprintf(bot_b, sizeof(bot_b), "CTS %03d°", (int)roundf(wrap_deg(snap.cts_deg)));
    } else if (active_mode == MARINE_MODE_ANCHOR_WATCH) {
        snprintf(top_b, sizeof(top_b), "%s", snap.anchor_dropped ? "ANCHORED" : "READY");
        snprintf(mid_b, sizeof(mid_b), "%dm / %um", (int)roundf(snap.anchor_dist_m), snap.anchor_radius_m);
        snprintf(bot_b, sizeof(bot_b), "%s", snap.gps_3d_fix ? "3D FIX" : "NO FIX");
    } else if (active_mode == MARINE_MODE_DIMMER) {
        uint8_t idx = dimmer_selected_group < snap.dimmer_group_count ? dimmer_selected_group : 0;
        const marine_dimmer_group_t *g = &snap.dimmer_groups[idx];
        snprintf(top_b, sizeof(top_b), "%s", g->name[0] ? g->name : "LIGHT");
        if (g->on && g->level_pct > 0) snprintf(mid_b, sizeof(mid_b), "%u%%", (unsigned)g->level_pct);
        else snprintf(mid_b, sizeof(mid_b), "OFF");
        snprintf(bot_b, sizeof(bot_b), "GRP %u/%u", (unsigned)(idx + 1), (unsigned)(snap.dimmer_group_count ? snap.dimmer_group_count : 1));
    } else if (active_mode == MARINE_MODE_MUSIC) {
        const marine_vol_target_t *t = &snap.vol_targets[0];
        snprintf(top_b, sizeof(top_b), "%s", t->name);
        snprintf(mid_b, sizeof(mid_b), "%u%%", (unsigned)t->pct);
        snprintf(bot_b, sizeof(bot_b), "%s", snap.play_state == MARINE_PLAY_PLAYING ? "PLAYING" : "PAUSED");
    } else if (active_mode == MARINE_MODE_SETTINGS) {
        snprintf(top_b, sizeof(top_b), "SETTINGS");
        snprintf(mid_b, sizeof(mid_b), "%d ITEMS", (int)KNOB_SET_COUNT);
        snprintf(bot_b, sizeof(bot_b), "2x MODES");
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

// A mode switched off on the MODES page is hidden. USB only with a USB host on the cable, SETTINGS not while an alarm is
// active (acknowledged or not).
static bool ring_slot_visible(int s) {
    marine_ui_mode_t m = ring_slot_modes[s];
    if (m == MARINE_MODE_AUTOPILOT) return snap.ap_mode;
    if (m == MARINE_MODE_DIMMER) return snap.lights_mode;
    if (m == MARINE_MODE_ANCHOR_WATCH) return snap.anchor_mode;
    if (m == MARINE_MODE_USB_HID) return snap.usb_mode && snap.usb_host_enumerated;
    if (m == MARINE_MODE_SETTINGS) return snap.active_alarm == MARINE_ALARM_NONE;
    if (m == MARINE_MODE_MUSIC) return snap.music_mode;
    if (m == MARINE_MODE_MOTOR) return snap.motor_mode;
    return true;
}

static void refresh_mode_ring(void) {
    lv_obj_set_style_border_color(ovl_mode_ring, snap.night_palette ? COL_NIGHT_DIM : lv_color_hex(0x6E86A6), 0);
    lv_obj_set_style_border_color(ring_center_cut, snap.night_palette ? COL_NIGHT_DIM : lv_color_hex(0x3D5270), 0);
    refresh_mode_ring_center_preview();
    int n = 0;
    for (int s = 0; s < MARINE_MODE_COUNT; s++) if (ring_slot_visible(s)) n++;
    // The position of the top slot among the visible ones: AUTO, or the first visible slot after it.
    int top = 0;
    for (int i = 0; i < MARINE_MODE_COUNT; i++) {
        int s = (RING_TOP_SLOT + i) % MARINE_MODE_COUNT;
        if (!ring_slot_visible(s)) continue;
        for (int j = 0; j < s; j++) if (ring_slot_visible(j)) top++;
        break;
    }
    int k = 0;
    for (int s = 0; s < MARINE_MODE_COUNT; s++) {
        if (!ring_slot_visible(s)) {
            lv_obj_add_flag(ring_slot_boxes[s], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float a = (float)(-M_PI / 2.0) + (float)(k - top) * 2.0f * (float)M_PI / (float)n;
        k++;
        lv_obj_align(ring_slot_boxes[s], LV_ALIGN_CENTER, (int)lroundf(86.0f * cosf(a)), (int)lroundf(86.0f * sinf(a)));
        lv_obj_remove_flag(ring_slot_boxes[s], LV_OBJ_FLAG_HIDDEN);

        bool sel = (s == ring_selected_slot);
        lv_obj_set_style_border_width(ring_slot_boxes[s], sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_bg_opa(ring_slot_boxes[s], sel ? LV_OPA_30 : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_shadow_width(ring_slot_boxes[s], (sel && snap.fx_glow_enabled) ? 16 : 0, 0);
        lv_obj_set_style_shadow_color(ring_slot_boxes[s], c_primary(), 0);
        lv_obj_set_style_text_color(ring_slot_labels[s], sel ? c_primary() : c_text(), 0);
        lv_obj_set_style_text_color(ring_slot_icons[s], sel ? c_primary() : c_text(), 0);
    }
}

static void open_mode_ring(void) {
    mode_ring_open = true;
    ring_opened_ms = snap.now_ms;
    for (uint8_t s = 0; s < MARINE_MODE_COUNT; s++) {
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
        marine_ui_mode_t was = active_mode;
        active_mode = ring_slot_modes[ring_selected_slot];
        if (active_mode != was) emit_action(active_mode, MARINE_ACT_MODE_CHANGED, active_mode, 0);
        if (active_mode == MARINE_MODE_SETTINGS) settings_enter();
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

    ap_wind_line = lv_line_create(scr);
    ap_wind_pts[0].x = 120; ap_wind_pts[0].y = 120;
    ap_wind_pts[1].x = 120; ap_wind_pts[1].y = 20;
    lv_line_set_points(ap_wind_line, ap_wind_pts, 2);
    lv_obj_set_style_line_width(ap_wind_line, 3, 0);
    lv_obj_set_style_line_rounded(ap_wind_line, true, 0);
    lv_obj_add_flag(ap_wind_line, LV_OBJ_FLAG_HIDDEN);

    ap_lbl_hdr = lv_label_create(scr);
    lv_label_set_text(ap_lbl_hdr, "HEADING");
    lv_obj_set_style_text_font(ap_lbl_hdr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ap_lbl_hdr, COL_TEXT_MUTED, 0);
    lv_obj_align(ap_lbl_hdr, LV_ALIGN_TOP_MID, 0, 26);

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

static void pilot_open_dialog(bool stale_only);

static bool ap_turn_allowed(void) {
    // Compass takes target changes; wind only once the provider supports them (ADJUST ON PILOT); route never.
    if (snap.ap_state == KNOB_AP_WIND) return snap.wind_adjust_supported;
    return snap.ap_state != KNOB_AP_ROUTE;
}

static void ap_on_input(marine_semantic_input_t ev, int32_t val) {
    if (ev == MARINE_INPUT_TURN && snap.ap_engaged && !ap_turn_allowed()) return;
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
            ap_confirm_dir = 0;
            ap_confirm_deadline_ms = snap.now_ms + 5000;
            lv_label_set_text(ap_confirm_lbl_title, "TURN RIGHT\nTO ENGAGE\nCOMPASS");
            lv_obj_remove_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        } else {
            ap_pending_active = false;
            ap_pending_sent = false;
            pilot_open_dialog(false);   // STANDBY first and focused: long press and one press is standby
        }
    }
}

// A dot on the rim at an angle relative to the bow (0 = ahead, negative to port).
static void ap_place_bug(float rel_deg, lv_color_t col) {
    float rad = (rel_deg - 90.0f) * (float)(M_PI / 180.0);
    lv_obj_set_pos(ap_cts_bug, 120 + (int)(cosf(rad) * 108.0f) - 5, 120 + (int)(sinf(rad) * 108.0f) - 5);
    lv_obj_set_style_bg_color(ap_cts_bug, col, 0);
}

// Wind (agreed mockup, ui-design 3.1): AWA large with P or S, the target in amber, the error and the heading, a cyan line from
// the centre for the AWA, an amber dot on the rim for the target.
static void ap_refresh_wind(void) {
    char buf[48], side, tside;
    int awa = knob_wind_abs(snap.wind_awa_deg, &side);
    lv_label_set_text(ap_lbl_hdr, "WIND \xE2\x80\xA2 AWA");
    snprintf(buf, sizeof(buf), "%d° %c", awa, side);
    lv_label_set_text(ap_lbl_hdg, buf);
    lv_obj_set_style_text_color(ap_lbl_hdg, c_text(), 0);
    lv_label_set_text(ap_lbl_mode, "WIND");
    lv_obj_set_style_text_color(ap_lbl_mode, c_primary(), 0);
    lv_obj_set_style_border_color(ap_pill_mode, c_primary(), 0);
    if (snap.wind_target_valid) {
        int tgt = knob_wind_abs(snap.wind_target_deg, &tside);
        snprintf(buf, sizeof(buf), "TGT %d° %c", tgt, tside);
        lv_label_set_text(ap_lbl_cts, buf);
        int off = (int)roundf(fabsf(knob_course_error(snap.wind_awa_deg, snap.wind_target_deg)));
        snprintf(buf, sizeof(buf), "%d° OFF \xE2\x80\xA2 HDG %03d°", off, (int)roundf(wrap_deg(snap.heading_deg)));
        ap_place_bug(snap.wind_target_deg, c_amber());
        lv_obj_remove_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(ap_lbl_cts, "TGT ---");
        snprintf(buf, sizeof(buf), "HDG %03d°", (int)roundf(wrap_deg(snap.heading_deg)));
        lv_obj_add_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(ap_lbl_cts, c_amber(), 0);
    lv_label_set_text(ap_lbl_delta, buf);
    lv_obj_set_style_text_color(ap_lbl_delta, c_muted(), 0);
    float rad = (snap.wind_awa_deg - 90.0f) * (float)(M_PI / 180.0);
    ap_wind_pts[1].x = 120 + cosf(rad) * 100.0f;
    ap_wind_pts[1].y = 120 + sinf(rad) * 100.0f;
    lv_line_set_points(ap_wind_line, ap_wind_pts, 2);
    lv_obj_set_style_line_color(ap_wind_line, c_primary(), 0);
    lv_obj_remove_flag(ap_wind_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);
    if (snap.wind_adjust_supported) snprintf(buf, sizeof(buf), "STEP %d° \xE2\x80\xA2 HOLD: PILOT", ap_step_deg);
    else snprintf(buf, sizeof(buf), "ADJUST ON PILOT");
    lv_label_set_text(ap_lbl_telem, buf);
    lv_obj_set_style_text_color(ap_lbl_telem, c_muted(), 0);
}

// Route (agreed mockup): the next waypoint, XTE large with the side to steer, a steer bar like the rudder bar (filled from the
// centre towards the side to steer), bearing, distance and heading. Turning does nothing.
static void ap_refresh_route(void) {
    char buf[48];
    lv_obj_add_flag(ap_wind_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
    snprintf(buf, sizeof(buf), "ROUTE \xE2\x80\xA2 TO %s", snap.route_wpt[0] ? snap.route_wpt : "---");
    lv_label_set_text(ap_lbl_hdr, buf);
    float xte_nm = fabsf(snap.route_xte_m) / 1852.0f;
    snprintf(buf, sizeof(buf), xte_nm < 10.0f ? "%.2fnm" : "%.0fnm", xte_nm);
    lv_label_set_text(ap_lbl_hdg, buf);
    lv_obj_set_style_text_color(ap_lbl_hdg, c_text(), 0);
    lv_label_set_text(ap_lbl_mode, "ROUTE");
    lv_obj_set_style_text_color(ap_lbl_mode, c_primary(), 0);
    lv_obj_set_style_border_color(ap_pill_mode, c_primary(), 0);
    snprintf(buf, sizeof(buf), "%03d° %.1fnm", (int)roundf(snap.route_brg_deg), snap.route_dist_nm);
    lv_label_set_text(ap_lbl_cts, buf);
    lv_obj_set_style_text_color(ap_lbl_cts, c_primary(), 0);
    int steer = knob_xte_steer(snap.route_xte_m, 10.0f);
    lv_label_set_text(ap_lbl_delta, steer > 0 ? "STEER RIGHT" : steer < 0 ? "STEER LEFT" : "ON TRACK");
    lv_obj_set_style_text_color(ap_lbl_delta, steer ? c_amber() : c_muted(), 0);
    if (steer == 0) {
        lv_obj_add_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);
    } else {
        int bw = (int)(xte_nm / 0.25f * 54.0f);   // full bar at a quarter mile off the track
        if (bw < 3) bw = 3;
        if (bw > 54) bw = 54;
        lv_obj_remove_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_width(ap_rudder_fill, bw);
        lv_obj_align(ap_rudder_fill, LV_ALIGN_CENTER, steer > 0 ? (bw / 2) : -(bw / 2), 0);
        lv_obj_set_style_bg_color(ap_rudder_fill, snap.night_palette ? COL_NIGHT_RED : (steer > 0 ? COL_GREEN : COL_RED), 0);
    }
    snprintf(buf, sizeof(buf), "HDG %03d° \xE2\x80\xA2 HOLD: PILOT", (int)roundf(wrap_deg(snap.heading_deg)));
    lv_label_set_text(ap_lbl_telem, buf);
    lv_obj_set_style_text_color(ap_lbl_telem, c_muted(), 0);
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
        lv_obj_add_flag(ap_wind_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(ap_cts_bug, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ap_rudder_fill, LV_OBJ_FLAG_HIDDEN);

    if (snap.ap_engaged && snap.ap_state == KNOB_AP_WIND) { ap_refresh_wind(); return; }
    if (snap.ap_engaged && snap.ap_state == KNOB_AP_ROUTE) { ap_refresh_route(); return; }
    lv_obj_add_flag(ap_wind_line, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(ap_lbl_hdr, "HEADING");

    snprintf(buf, sizeof(buf), "%03d°%s", (int)roundf(wrap_deg(snap.heading_deg)), heading_ref_str());
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
        snprintf(buf, sizeof(buf), "COMPASS • STEP %d°", ap_step_deg);
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
// --- Screen 2: Anchor. The watch circle is the plot (design B in docs/ui-design.md): ring = the chosen radius, boat dot =
// distance and bearing from the anchor, a short trail of earlier positions, flat numbers, straight state and telemetry lines. ---
#define ANC_R 72                     // watch circle radius on screen, px
#define ANC_TRAIL_STEP_MS 3000

static float anc_hist_ratio[4], anc_hist_brg[4];
static uint8_t anc_hist_n = 0;
static uint32_t anc_hist_ms = 0;

static void anc_show(lv_obj_t *o, bool on) {
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t* anc_make_ring(lv_obj_t *scr, int diameter, bool filled, int border_w) {
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, diameter, diameter);
    lv_obj_center(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    if (filled) lv_obj_set_style_bg_color(o, COL_CARD, 0);
    else lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, border_w, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// Sets text and colour only when they changed: the anchor screen refreshes several times a second.
static void anc_set_line(lv_obj_t *l, const char *text, lv_color_t col) {
    if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, col, 0);
}

// Top status line of the boot and setup screens: straight 20 px, like the anchor screen's.
static void sys_set_state(const char *text, lv_color_t col) {
    if (strcmp(lv_label_get_text(sys_lbl_state), text) != 0) lv_label_set_text(sys_lbl_state, text);
    lv_obj_set_style_text_color(sys_lbl_state, col, 0);
}

static lv_obj_t* anc_make_label(lv_obj_t *scr, const lv_font_t *font, int y_off) {
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y_off);
    return l;
}

static lv_obj_t* build_anchor_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    anc_ring = anc_make_ring(scr, ANC_R * 2, true, 2);
    anc_ring_half = anc_make_ring(scr, ANC_R, false, 1);
    lv_obj_set_style_border_color(anc_ring_half, lv_color_hex(0x233856), 0);
    anc_ring_prev = anc_make_ring(scr, ANC_R * 2, false, 3);
    lv_obj_add_flag(anc_ring_prev, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) {
        int d = 6 - i;
        anc_trail[i] = lv_obj_create(scr);
        lv_obj_set_size(anc_trail[i], d, d);
        lv_obj_set_style_radius(anc_trail[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(anc_trail[i], COL_GREEN, 0);
        lv_obj_set_style_bg_opa(anc_trail[i], (lv_opa_t)(LV_OPA_70 - i * 40), 0);
        lv_obj_set_style_border_width(anc_trail[i], 0, 0);
        lv_obj_remove_flag(anc_trail[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(anc_trail[i], LV_OBJ_FLAG_HIDDEN);
    }

    anc_icon = icon_canvas_create(scr, 24, &anc_icon_buf);
    lv_obj_center(anc_icon);

    anc_boat_dot = lv_obj_create(scr);
    lv_obj_set_size(anc_boat_dot, 12, 12);
    lv_obj_set_style_radius(anc_boat_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(anc_boat_dot, COL_GREEN, 0);
    lv_obj_set_style_border_color(anc_boat_dot, COL_TEXT_MAIN, 0);
    lv_obj_set_style_border_width(anc_boat_dot, 2, 0);
    lv_obj_remove_flag(anc_boat_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    anc_lbl_radius = anc_make_label(scr, &lv_font_montserrat_12, -58);
    anc_lbl_dist = anc_make_label(scr, &lv_font_montserrat_14, 0);
    anc_lbl_center = anc_make_label(scr, &lv_font_montserrat_12, 36);
    anc_lbl_hint = anc_make_label(scr, &lv_font_montserrat_12, 54);
    anc_lbl_big = anc_make_label(scr, &lv_font_montserrat_28, 38);

    // State and telemetry in straight lines just outside the watch circle (it spans y 48 to 192), the state in 20 px, the
    // telemetry in 14 px (owner, 2026-11-23). At y 30 the round edge leaves about 130 px, 28 px would reach the rim.
    anc_lbl_state = anc_make_label(scr, &lv_font_montserrat_20, -90);
    anc_lbl_telem = anc_make_label(scr, &lv_font_montserrat_14, 90);
    lv_label_set_long_mode(anc_lbl_telem, LV_LABEL_LONG_MODE_DOTS);   // the round edge leaves about 160 px at y 210
    lv_obj_set_width(anc_lbl_telem, 160);
    lv_obj_set_style_text_align(anc_lbl_telem, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(anc_lbl_telem, LV_ALIGN_CENTER, 0, 90);
    return scr;
}

// Centre of the object on the plot: ratio = distance / radius (1.0 = on the circle), bearing from the anchor, north up.
static void anc_place(lv_obj_t *o, float ratio, float bearing_deg) {
    if (ratio > 1.1f) ratio = 1.1f;
    float th = (bearing_deg - 90.0f) * (float)(M_PI / 180.0);
    lv_obj_align(o, LV_ALIGN_CENTER, (int)lroundf(cosf(th) * ratio * ANC_R), (int)lroundf(sinf(th) * ratio * ANC_R));
}

// Bottom line: only what this boat publishes. A value that never arrived is left out, a stale one shows ---.
static void anchor_telemetry(char *out, size_t n, uint32_t dbits) {
    char part[3][16];
    int k = 0;
    if (snap.depth_updated_ms != 0) {
        if (dbits & SB_DEPTH) snprintf(part[k++], sizeof(part[0]), "---m");
        else snprintf(part[k++], sizeof(part[0]), "%.1fm", snap.depth_m);
    }
    if (snap.voltage_updated_ms != 0) {
        if (dbits & SB_VOLTAGE) snprintf(part[k++], sizeof(part[0]), "---V");
        else snprintf(part[k++], sizeof(part[0]), "%.1fV", snap.voltage_v);
    }
    if (dbits & SB_GPS) snprintf(part[k++], sizeof(part[0]), "---");
    else snprintf(part[k++], sizeof(part[0]), "%s", snap.gps_3d_fix ? "3D FIX" : "NO FIX");
    out[0] = '\0';
    for (int i = 0; i < k; i++) {
        if (i) strncat(out, " \xE2\x80\xA2 ", n - strlen(out) - 1);   // " • "
        strncat(out, part[i], n - strlen(out) - 1);
    }
}

static void anchor_menu_render(void) {
    static const int16_t y3[3] = { -42, -8, 26 };   // item centres relative to the screen centre
    static const int16_t y2[2] = { -24, 10 };
    const int16_t *ys = (anc_menu_count == 3) ? y3 : y2;
    for (int i = 0; i < 3; i++) {
        if (i >= anc_menu_count) { lv_obj_add_flag(anc_menu_items[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(anc_menu_items[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(anc_menu_items[i], anc_items[i].text);
        bool sel = (i == anchor_menu_idx);
        lv_obj_set_style_text_color(anc_menu_items[i], sel ? COL_BG : (anc_items[i].danger ? c_red() : c_text()), 0);
        lv_obj_align(anc_menu_items[i], LV_ALIGN_CENTER, 0, ys[i]);
    }
    lv_obj_set_style_bg_color(anc_menu_pill, c_amber(), 0);
    lv_obj_align(anc_menu_pill, LV_ALIGN_CENTER, 0, ys[anchor_menu_idx]);
}

// The dialog opened by a long press: the actions that are valid in the current state, and Cancel.
static void anchor_menu_open_dialog(void) {
    anchor_menu_open = true;
    anchor_menu_idx = 0;
    anchor_menu_opened_ms = snap.now_ms;
    uint16_t r = anchor_preview_active ? anchor_preview_radius : snap.anchor_radius_m;
    char b[40];
    if (snap.anchor_dropped) {
        lv_label_set_text(anc_menu_title, "ANCHOR ACTIONS");
        lv_label_set_text(anc_menu_info, "");
        anc_items[0] = (anc_item_t){ "MOVE DROP POINT HERE", MARINE_ACT_ANCHOR_MOVE_DROP, false, false };
        anc_items[1] = (anc_item_t){ "RAISE ANCHOR", MARINE_ACT_ANCHOR_RAISE, false, true };
        anc_items[2] = (anc_item_t){ "CANCEL", MARINE_ACT_ANCHOR_RAISE, true, false };
        anc_menu_count = 3;
        anchor_menu_idx = 1;   // RAISE ANCHOR is focused (owner, 2026-11-25)
    } else {
        lv_label_set_text(anc_menu_title, "DROP ANCHOR");
        snprintf(b, sizeof(b), "%u m circle at this position", (unsigned)r);
        lv_label_set_text(anc_menu_info, b);
        anc_items[0] = (anc_item_t){ "DROP HERE", MARINE_ACT_ANCHOR_DROP, false, false };
        anc_items[1] = (anc_item_t){ "CANCEL", MARINE_ACT_ANCHOR_DROP, true, false };
        anc_menu_count = 2;
    }
    lv_label_set_text(anc_menu_hint, "TURN \xE2\x80\xA2 PRESS");   // TURN • PRESS
    lv_obj_set_style_text_color(anc_menu_title, c_amber(), 0);
    lv_obj_set_style_text_color(anc_menu_info, c_muted(), 0);
    lv_obj_set_style_text_color(anc_menu_hint, c_muted(), 0);
    lv_obj_set_style_border_color(ovl_anc_menu, c_amber(), 0);
    anchor_menu_render();
    lv_obj_remove_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
}

static void anchor_pending_close(void) {
    anc_pend = ANC_PEND_NONE;
    lv_obj_add_flag(ovl_anc_res, LV_OBJ_FLAG_HIDDEN);
    if (active_mode == MARINE_MODE_ANCHOR_WATCH) anchor_refresh();
}

static void anchor_dialogs_close(void) {
    if (anchor_menu_open) {
        anchor_menu_open = false;
        lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
    }
    if (anc_pend != ANC_PEND_NONE) {
        anc_pend = ANC_PEND_NONE;
        lv_obj_add_flag(ovl_anc_res, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anchor_pending_refuse(uint8_t code) {    anc_pend = ANC_PEND_REFUSED;
    anc_pend_until_ms = snap.now_ms + 4000;
    const char *t = "NOT SENT", *s1 = "", *s2 = "NOTHING WAS CHANGED";
    switch (code) {
    case MARINE_RESULT_LIVE_OFF: s1 = "CONTROL IS OFF"; break;
    case MARINE_RESULT_NO_LINK: s1 = "NO LINK TO THE SERVER"; break;
    case MARINE_RESULT_NO_POSITION: s1 = "NO POSITION FIX"; break;
    case MARINE_RESULT_FAILED: t = "FAILED"; s1 = "THE SERVER REFUSED IT"; s2 = "CHECK THE ANCHOR PLUGIN"; break;
    default: t = "NOT CONFIRMED"; s1 = "NO ANSWER FROM THE SERVER"; s2 = "CHECK THE ANCHOR SCREEN"; break;
    }
    lv_obj_set_style_border_color(ovl_anc_res, c_red(), 0);
    lv_obj_add_flag(anc_res_arc, LV_OBJ_FLAG_HIDDEN);
    icon_recolor(anc_res_icon, ICON_CROSS, 52, c_red(), &anc_res_icon_col);
    lv_obj_remove_flag(anc_res_icon, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(anc_res_t, t);
    lv_obj_set_style_text_color(anc_res_t, c_red(), 0);
    lv_label_set_text(anc_res_s1, s1);
    lv_label_set_text(anc_res_s2, s2);
    lv_obj_remove_flag(ovl_anc_res, LV_OBJ_FLAG_HIDDEN);
}

static void anchor_start_action(marine_ui_action_type_t act, int32_t value) {
    char b[40];
    const char *t = "WORKING...", *s1 = "";
    switch (act) {
    case MARINE_ACT_ANCHOR_DROP:
        t = "DROPPING...";
        snprintf(b, sizeof(b), "%u m AT THIS POSITION", (unsigned)snap.anchor_radius_m);
        s1 = b;
        break;
    case MARINE_ACT_ANCHOR_MOVE_DROP: t = "MOVING DROP POINT..."; s1 = "TO THIS POSITION"; break;
    case MARINE_ACT_ANCHOR_RAISE: t = "RAISING..."; s1 = "THE WATCH STOPS"; break;
    case MARINE_ACT_ANCHOR_SET_RADIUS:
        t = "SETTING RADIUS...";
        snprintf(b, sizeof(b), "TO %ld m", (long)value);
        s1 = b;
        break;
    default: break;
    }
    anc_pend = ANC_PEND_WAIT;
    anc_pend_act = act;
    anc_pend_start_ms = snap.now_ms;
    anc_pend_ok = false;
    anc_pend_expect = (act == MARINE_ACT_ANCHOR_DROP) ? 1 : (act == MARINE_ACT_ANCHOR_RAISE ? 0 : -1);
    anc_seen_seq = snap.action_result_seq;
    lv_obj_set_style_border_color(ovl_anc_res, c_amber(), 0);
    lv_obj_add_flag(anc_res_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_arc_color(anc_res_arc, c_amber(), LV_PART_INDICATOR);
    lv_obj_remove_flag(anc_res_arc, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(anc_res_t, t);
    lv_obj_set_style_text_color(anc_res_t, c_text(), 0);
    lv_label_set_text(anc_res_s1, s1);
    lv_label_set_text(anc_res_s2, "");
    lv_obj_remove_flag(ovl_anc_res, LV_OBJ_FLAG_HIDDEN);
    emit_action(MARINE_MODE_ANCHOR_WATCH, act, value, 0);
}

// Every tick: the result of the last action arrives in the snapshot, the state change follows through the data stream.
static void anchor_pending_update(void) {
    if (anc_pend == ANC_PEND_NONE) return;
    uint32_t now = snap.now_ms;
    if (anc_pend == ANC_PEND_REFUSED) {
        if (now >= anc_pend_until_ms) anchor_pending_close();
        return;
    }
    lv_arc_set_rotation(anc_res_arc, (int32_t)((now / 3U) % 360U));
    if (snap.action_result_seq != anc_seen_seq) {
        anc_seen_seq = snap.action_result_seq;
        if (snap.action_result == MARINE_RESULT_OK) {
            anc_pend_ok = true;
            anc_pend_ok_ms = now;
        } else {
            anchor_pending_refuse(snap.action_result);
            return;
        }
    }
    bool done = (anc_pend_expect < 0) ? anc_pend_ok : (snap.anchor_dropped == (anc_pend_expect == 1));
    if (done) {
        anchor_pending_close();
    } else if (anc_pend_ok ? (now - anc_pend_ok_ms > 6000U) : (now - anc_pend_start_ms > 8000U)) {
        anchor_pending_refuse(0);
    }
}

static void anchor_menu_choose(void) {
    anc_item_t it = anc_items[anchor_menu_idx];
    anchor_menu_open = false;
    lv_obj_add_flag(ovl_anc_menu, LV_OBJ_FLAG_HIDDEN);
    if (it.cancel) return;
    if (it.act == MARINE_ACT_ANCHOR_DROP && anchor_preview_active) {
        // the radius turned to but not applied yet is the one to drop with
        emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_SET_RADIUS, anchor_preview_radius, 0);
        anchor_preview_active = false;
    }
    anchor_start_action(it.act, 1);
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
        if (snap.anchor_dropped) anchor_start_action(MARINE_ACT_ANCHOR_SET_RADIUS, r);   // a server call
        else emit_action(MARINE_MODE_ANCHOR_WATCH, MARINE_ACT_ANCHOR_SET_RADIUS, r, 0); // only remembered for the next drop
        anchor_refresh();
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        anchor_menu_open_dialog();
    }
}

static void anchor_refresh(void) {
    char top_buf[48], bot_buf[48], tmp[32];
    bool mask = (current_stale() != STALE_NONE);   // essential anchor data (or the link) stale: mask the mode
    uint32_t dbits = display_stale_bits();          // display-only values stale: mask just those

    anc_show(anc_ring_prev, false);
    anc_show(anc_icon, false);
    anc_show(anc_boat_dot, false);
    for (int i = 0; i < 3; i++) anc_show(anc_trail[i], false);
    anc_show(anc_lbl_radius, false);
    anc_show(anc_lbl_dist, false);
    anc_show(anc_lbl_center, false);
    anc_show(anc_lbl_hint, false);
    anc_show(anc_lbl_big, false);

    if (mask) {
        lv_obj_set_style_border_color(anc_ring, lv_color_hex(0x233856), 0);
        lv_obj_set_style_border_opa(anc_ring, LV_OPA_COVER, 0);
        anc_show(anc_ring_half, false);
        anc_set_line(anc_lbl_state, "ANCHOR", c_muted());
        anc_set_line(anc_lbl_telem, "---", c_muted());
        return;
    }
    anc_show(anc_ring_half, true);

    bool eng = snap.anchor_dropped;
    bool prev = anchor_preview_active;
    uint16_t cur_r = snap.anchor_radius_m ? snap.anchor_radius_m : 35;

    // The ring is the chosen radius. While turning, the new radius is a second ring and the old one fades back.
    lv_obj_set_style_border_color(anc_ring, eng ? c_primary() : lv_color_hex(0x284166), 0);
    lv_obj_set_style_border_opa(anc_ring, prev ? LV_OPA_40 : LV_OPA_COVER, 0);

    const char *top; lv_color_t top_col;
    if (prev) { top = "SET RADIUS"; top_col = c_amber(); }
    else if (eng) { top = "DROPPED"; top_col = c_primary(); }
    else { top = "RAISED"; top_col = c_muted(); }
    snprintf(top_buf, sizeof(top_buf), "%s", top);

    if (prev) {
        int px = (int)lroundf((float)ANC_R * (float)anchor_preview_radius / (float)cur_r);
        if (px < 24) px = 24;
        if (px > 100) px = 100;
        lv_obj_set_size(anc_ring_prev, px * 2, px * 2);
        lv_obj_center(anc_ring_prev);
        lv_obj_set_style_border_color(anc_ring_prev, c_amber(), 0);
        anc_show(anc_ring_prev, true);
        snprintf(tmp, sizeof(tmp), "%u m", (unsigned)anchor_preview_radius);
        lv_label_set_text(anc_lbl_big, tmp);
        lv_obj_set_style_text_color(anc_lbl_big, c_amber(), 0);
        lv_obj_align(anc_lbl_big, LV_ALIGN_CENTER, 0, 38);
        anc_show(anc_lbl_big, true);
        lv_label_set_text(anc_lbl_center, "PRESS TO APPLY");
        lv_obj_set_style_text_color(anc_lbl_center, c_amber(), 0);
        lv_obj_align(anc_lbl_center, LV_ALIGN_CENTER, 0, 62);
        anc_show(anc_lbl_center, true);
    } else {
        snprintf(tmp, sizeof(tmp), "%u m", (unsigned)cur_r);
        lv_label_set_text(anc_lbl_radius, tmp);
        lv_obj_set_style_text_color(anc_lbl_radius, c_muted(), 0);
        anc_show(anc_lbl_radius, true);
    }

    if (eng) {
        icon_recolor(anc_icon, ICON_ANCHOR, 24, c_amber(), &anc_icon_col);
        anc_show(anc_icon, true);
        if (snap.gps_3d_fix) {
            float ratio = snap.anchor_dist_m / (float)cur_r;
            float brg = snap.anchor_bearing_deg;
            if (anc_hist_n == 0 || snap.now_ms - anc_hist_ms >= ANC_TRAIL_STEP_MS) {
                for (int i = 3; i > 0; i--) { anc_hist_ratio[i] = anc_hist_ratio[i - 1]; anc_hist_brg[i] = anc_hist_brg[i - 1]; }
                anc_hist_ratio[0] = ratio; anc_hist_brg[0] = brg;
                if (anc_hist_n < 4) anc_hist_n++;
                anc_hist_ms = snap.now_ms;
            } else {
                anc_hist_ratio[0] = ratio; anc_hist_brg[0] = brg;   // the newest sample follows the dot until the next step
            }
            for (int i = 0; i < 3; i++) {
                if (i + 1 < anc_hist_n) { anc_place(anc_trail[i], anc_hist_ratio[i + 1], anc_hist_brg[i + 1]); anc_show(anc_trail[i], true); }
            }
            anc_place(anc_boat_dot, ratio, brg);
            anc_show(anc_boat_dot, true);
            if (!prev) {
                snprintf(tmp, sizeof(tmp), "%d m", (int)lroundf(snap.anchor_dist_m));
                lv_label_set_text(anc_lbl_dist, tmp);
                lv_obj_set_style_text_color(anc_lbl_dist, c_text(), 0);
                // beside the dot, on the side away from the anchor so the glyph stays clear
                bool right = lv_obj_get_x_aligned(anc_boat_dot) >= 0;
                lv_obj_align_to(anc_lbl_dist, anc_boat_dot, right ? LV_ALIGN_OUT_RIGHT_MID : LV_ALIGN_OUT_LEFT_MID, right ? 4 : -4, 0);
                anc_show(anc_lbl_dist, true);
            }
        } else if (!prev) {
            lv_label_set_text(anc_lbl_dist, "NO FIX");
            lv_obj_set_style_text_color(anc_lbl_dist, c_amber(), 0);
            lv_obj_align(anc_lbl_dist, LV_ALIGN_CENTER, 0, 30);
            anc_show(anc_lbl_dist, true);
        }
    } else {
        anc_hist_n = 0;
        lv_obj_align(anc_boat_dot, LV_ALIGN_CENTER, 0, 0);   // the boat is the drop point
        anc_show(anc_boat_dot, true);
        if (!prev) {
            lv_label_set_text(anc_lbl_center, "HOLD TO DROP");
            lv_obj_set_style_text_color(anc_lbl_center, c_text(), 0);
            lv_obj_align(anc_lbl_center, LV_ALIGN_CENTER, 0, 36);
            anc_show(anc_lbl_center, true);
            lv_label_set_text(anc_lbl_hint, "TURN TO SET RADIUS");
            lv_obj_set_style_text_color(anc_lbl_hint, c_muted(), 0);
            anc_show(anc_lbl_hint, true);
        }
    }

    anchor_telemetry(bot_buf, sizeof(bot_buf), dbits);
    anc_set_line(anc_lbl_state, top_buf, top_col);
    anc_set_line(anc_lbl_telem, bot_buf, c_muted());
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
        if (grp->onoff_only) {
            // A relay: one click is enough. Right = on, left = off. Nothing to do when it already is that way.
            uint8_t want = val > 0 ? 100 : 0;
            uint8_t cur = dimmer_pending_active ? dimmer_pending_level : (grp->on ? 100 : 0);
            if (want == cur) return;
            dimmer_pending_level = want;
            dimmer_pending_active = true;
            dimmer_pending_sent = false;
            dimmer_pending_deadline_ms = snap.now_ms + 250;
            dimmer_refresh();
            return;
        }
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
        if (g->onoff_only) {
            lv_label_set_text(dim_lbl_val, "ON");
            lv_label_set_text(dim_lbl_hint, "TURN: ON/OFF • 1x: NEXT");
        } else {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)lvl);
            lv_label_set_text(dim_lbl_val, buf);
            lv_label_set_text(dim_lbl_hint, "1x: GROUP • HOLD: OFF");
        }
    } else {
        lv_label_set_text(dim_lbl_val, "OFF");
        lv_label_set_text(dim_lbl_hint, g->onoff_only ? "TURN RIGHT: ON" : "HOLD TO TURN ON");
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

    lv_obj_t *center_cut = ring_center_cut = lv_obj_create(ovl_mode_ring);
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

    for (int s = 0; s < MARINE_MODE_COUNT; s++) {   // positions are set by refresh_mode_ring() from the visible slots
        ring_slot_boxes[s] = lv_obj_create(ovl_mode_ring);
        lv_obj_set_size(ring_slot_boxes[s], 62, 48);          // icon (knob_icon_28) above the label
        lv_obj_set_style_radius(ring_slot_boxes[s], 6, 0);
        lv_obj_set_style_pad_all(ring_slot_boxes[s], 0, 0);
        lv_obj_align(ring_slot_boxes[s], LV_ALIGN_CENTER, 0, -86);
        lv_obj_remove_flag(ring_slot_boxes[s], LV_OBJ_FLAG_SCROLLABLE);

        // In ring order: propeller, ship's wheel, anchor, light bulb, music notes, USB, gear.
        static const char *const slot_icons[MARINE_MODE_COUNT] = { "\xEF\xA1\xA3", "\xEF\x99\x95", "\xEF\x84\xBD", "\xEF\x83\xAB",
                                                                   "\xEF\x80\x81", "\xEF\x8A\x87", "\xEF\x80\x93" };
        ring_slot_icons[s] = lv_label_create(ring_slot_boxes[s]);
        lv_obj_set_style_text_font(ring_slot_icons[s], &knob_icon_28, 0);
        lv_label_set_text(ring_slot_icons[s], slot_icons[s]);
        lv_obj_align(ring_slot_icons[s], LV_ALIGN_TOP_MID, 0, 2);

        ring_slot_labels[s] = lv_label_create(ring_slot_boxes[s]);
        lv_label_set_text(ring_slot_labels[s], mode_registry[ring_slot_modes[s]].ring_label);
        lv_obj_set_style_text_font(ring_slot_labels[s], &lv_font_montserrat_12, 0);
        lv_obj_align(ring_slot_labels[s], LV_ALIGN_BOTTOM_MID, 0, -2);

    }

    // 2. Full-Screen Alarm Takeover
    ovl_alarm = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_alarm, 240, 240);
    lv_obj_center(ovl_alarm);
    lv_obj_set_style_radius(ovl_alarm, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_alarm, COL_BG, 0);
    lv_obj_set_style_bg_opa(ovl_alarm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ovl_alarm, 6, 0);
    lv_obj_set_style_border_color(ovl_alarm, COL_RED, 0);
    lv_obj_set_style_pad_all(ovl_alarm, 0, 0);
    lv_obj_remove_flag(ovl_alarm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_alarm, LV_OBJ_FLAG_HIDDEN);

    alm_lbl_title = anc_make_label(ovl_alarm, &lv_font_montserrat_20, -74);
    alm_icon = anc_make_label(ovl_alarm, &knob_icon_96, -6);
    alm_lbl_dist = anc_make_label(ovl_alarm, &lv_font_montserrat_20, 58);
    alm_lbl_ack = anc_make_label(ovl_alarm, &lv_font_montserrat_14, 83);

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

    ap_confirm_lbl_title = lv_label_create(ovl_ap_confirm);
    lv_label_set_text(ap_confirm_lbl_title, "TURN RIGHT\nTO CONFIRM");
    lv_obj_set_style_text_align(ap_confirm_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ap_confirm_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ap_confirm_lbl_title, COL_CYAN, 0);
    lv_obj_align(ap_confirm_lbl_title, LV_ALIGN_CENTER, 0, -10);

    // Pilot dialog (#7), styled like the anchor dialog
    ovl_pilot = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_pilot, 240, 240);
    lv_obj_center(ovl_pilot);
    lv_obj_set_style_radius(ovl_pilot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_pilot, COL_BG, 0);
    lv_obj_set_style_bg_opa(ovl_pilot, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ovl_pilot, 3, 0);
    lv_obj_remove_flag(ovl_pilot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_pilot, LV_OBJ_FLAG_HIDDEN);
    pilot_title = anc_make_label(ovl_pilot, &lv_font_montserrat_12, -92);
    pilot_pill = lv_obj_create(ovl_pilot);   // behind the item labels: created first
    lv_obj_set_size(pilot_pill, 180, 24);
    lv_obj_set_style_radius(pilot_pill, 12, 0);
    lv_obj_set_style_border_width(pilot_pill, 0, 0);
    lv_obj_remove_flag(pilot_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 7; i++) pilot_items[i] = anc_make_label(ovl_pilot, &lv_font_montserrat_14, 0);
    pilot_hint = anc_make_label(ovl_pilot, &lv_font_montserrat_12, 94);

    ap_confirm_lbl_sec = lv_label_create(ovl_ap_confirm);
    lv_obj_set_style_text_font(ap_confirm_lbl_sec, &lv_font_montserrat_12, 0);
    lv_obj_align(ap_confirm_lbl_sec, LV_ALIGN_CENTER, 0, 34);

    // 5. Anchor dialog (a choice of actions) and the pending / refused screen that follows it
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

    anc_menu_title = anc_make_label(ovl_anc_menu, &lv_font_montserrat_12, -80);
    anc_menu_info = anc_make_label(ovl_anc_menu, &lv_font_montserrat_12, -60);
    anc_menu_pill = lv_obj_create(ovl_anc_menu);   // behind the item labels: created first
    lv_obj_set_size(anc_menu_pill, 190, 30);
    lv_obj_set_style_radius(anc_menu_pill, 15, 0);
    lv_obj_set_style_bg_color(anc_menu_pill, COL_AMBER, 0);
    lv_obj_set_style_border_width(anc_menu_pill, 0, 0);
    lv_obj_remove_flag(anc_menu_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 3; i++) {
        anc_menu_items[i] = anc_make_label(ovl_anc_menu, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(anc_menu_items[i], COL_TEXT_MAIN, 0);
    }
    anc_menu_hint = anc_make_label(ovl_anc_menu, &lv_font_montserrat_12, 84);

    ovl_anc_res = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_anc_res, 240, 240);
    lv_obj_center(ovl_anc_res);
    lv_obj_set_style_radius(ovl_anc_res, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ovl_anc_res, COL_BG, 0);
    lv_obj_set_style_bg_opa(ovl_anc_res, LV_OPA_90, 0);
    lv_obj_set_style_border_width(ovl_anc_res, 3, 0);
    lv_obj_set_style_border_color(ovl_anc_res, COL_AMBER, 0);
    lv_obj_remove_flag(ovl_anc_res, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_anc_res, LV_OBJ_FLAG_HIDDEN);

    anc_res_arc = lv_arc_create(ovl_anc_res);   // the spinner: a quarter of a circle that is turned every tick
    lv_obj_set_size(anc_res_arc, 56, 56);
    lv_obj_align(anc_res_arc, LV_ALIGN_CENTER, 0, -24);
    lv_arc_set_bg_angles(anc_res_arc, 0, 360);
    lv_arc_set_range(anc_res_arc, 0, 100);
    lv_arc_set_value(anc_res_arc, 25);
    lv_obj_remove_style(anc_res_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(anc_res_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(anc_res_arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(anc_res_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(anc_res_arc, lv_color_hex(0x233856), LV_PART_MAIN);
    lv_obj_set_style_arc_color(anc_res_arc, COL_AMBER, LV_PART_INDICATOR);

    anc_res_icon = icon_canvas_create(ovl_anc_res, 52, &anc_res_icon_buf);
    lv_obj_align(anc_res_icon, LV_ALIGN_CENTER, 0, -24);
    lv_obj_add_flag(anc_res_icon, LV_OBJ_FLAG_HIDDEN);
    anc_res_t = anc_make_label(ovl_anc_res, &lv_font_montserrat_14, 24);
    anc_res_s1 = anc_make_label(ovl_anc_res, &lv_font_montserrat_12, 46);
    anc_res_s2 = anc_make_label(ovl_anc_res, &lv_font_montserrat_12, 64);
    lv_obj_set_style_text_color(anc_res_t, COL_TEXT_MAIN, 0);
    lv_obj_set_style_text_color(anc_res_s1, COL_TEXT_MUTED, 0);
    lv_obj_set_style_text_color(anc_res_s2, COL_TEXT_MUTED, 0);
    // 6. Boot and pairing system screen (tick ring, straight status lines, pill)
    scr_system = lv_obj_create(NULL);
    lv_obj_set_size(scr_system, 240, 240);
    lv_obj_set_style_bg_color(scr_system, COL_BG, 0);
    lv_obj_remove_flag(scr_system, LV_OBJ_FLAG_SCROLLABLE);

    build_radial_tick_ring(scr_system, sys_tick_lines, sys_tick_pts);
    update_radial_tick_ring(sys_tick_lines, sys_tick_pts, 0.0f, true, true);

    sys_lbl_state = lv_label_create(scr_system);
    lv_label_set_text(sys_lbl_state, "");
    lv_obj_set_style_text_font(sys_lbl_state, &lv_font_montserrat_20, 0);
    lv_obj_align(sys_lbl_state, LV_ALIGN_CENTER, 0, -90);
    sys_lbl_bottom = anc_make_label(scr_system, &lv_font_montserrat_12, 92);

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

    sys_pair_big = lv_label_create(scr_system);
    lv_obj_set_style_text_font(sys_pair_big, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(sys_pair_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(sys_pair_big, 2, 0);
    lv_label_set_text(sys_pair_big, "PAIRING\nWAITING FOR\nAPPROVAL");
    lv_obj_align(sys_pair_big, LV_ALIGN_CENTER, 0, -22);
    lv_obj_add_flag(sys_pair_big, LV_OBJ_FLAG_HIDDEN);

    sys_pair_hint = lv_label_create(scr_system);
    lv_obj_set_style_text_font(sys_pair_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(sys_pair_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sys_pair_hint, "APPROVE IN SIGNALK ADMIN UI");
    lv_obj_align(sys_pair_hint, LV_ALIGN_CENTER, 0, 52);
    lv_obj_add_flag(sys_pair_hint, LV_OBJ_FLAG_HIDDEN);

    sys_pair_name = lv_label_create(scr_system);
    lv_obj_set_style_text_font(sys_pair_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(sys_pair_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(sys_pair_name, "");
    lv_obj_align(sys_pair_name, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_flag(sys_pair_name, LV_OBJ_FLAG_HIDDEN);
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
static const char *pilot_line_text(knob_pilot_line_t l) {
    switch (l) {
    case KNOB_PL_STANDBY: return "STANDBY";
    case KNOB_PL_COMPASS: return "COMPASS";
    case KNOB_PL_WIND: return "WIND";
    case KNOB_PL_ROUTE: return "ROUTE";
    case KNOB_PL_TACK_PORT: return "TACK PORT";
    case KNOB_PL_TACK_STBD: return "TACK STARBOARD";
    default: return "CANCEL";
    }
}

static void pilot_render(void) {
    int top = -((int)pilot_count - 1) * 13;   // 26 px per line, centred
    for (int i = 0; i < 7; i++) {
        if (i >= pilot_count) { lv_obj_add_flag(pilot_items[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(pilot_items[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(pilot_items[i], pilot_line_text(pilot_lines[i]));
        bool sel = i == pilot_idx;
        lv_obj_set_style_text_color(pilot_items[i], sel ? COL_BG : (pilot_lines[i] == KNOB_PL_CANCEL ? c_muted() : c_text()), 0);
        lv_obj_align(pilot_items[i], LV_ALIGN_CENTER, 0, top + i * 26);
    }
    lv_obj_set_style_bg_color(pilot_pill, c_amber(), 0);
    lv_obj_align(pilot_pill, LV_ALIGN_CENTER, 0, top + pilot_idx * 26);
}

static void pilot_close(void) {
    if (!pilot_open) return;
    pilot_open = false;
    lv_obj_add_flag(ovl_pilot, LV_OBJ_FLAG_HIDDEN);
}

// Long press while engaged. With the pilot's data stale only STANDBY and CANCEL are offered (ui-design section 3.1).
static void pilot_open_dialog(bool stale_only) {
    static const char *const names[] = { "STANDBY", "COMPASS", "WIND", "ROUTE", "?" };
    knob_ap_state_t st = snap.ap_state <= KNOB_AP_UNKNOWN ? (knob_ap_state_t)snap.ap_state : KNOB_AP_UNKNOWN;
    if (stale_only) {
        pilot_lines[0] = KNOB_PL_STANDBY;
        pilot_lines[1] = KNOB_PL_CANCEL;
        pilot_count = 2;
    } else {
        pilot_count = (uint8_t)knob_pilot_lines(st == KNOB_AP_UNKNOWN ? KNOB_AP_AUTO : st, pilot_lines, 7);
    }
    pilot_idx = 0;
    pilot_opened_ms = snap.now_ms;
    char b[32];
    snprintf(b, sizeof(b), "PILOT \xE2\x80\xA2 %s", names[st]);
    lv_label_set_text(pilot_title, b);
    lv_obj_set_style_text_color(pilot_title, c_amber(), 0);
    lv_label_set_text(pilot_hint, "TURN \xE2\x80\xA2 PRESS");
    lv_obj_set_style_text_color(pilot_hint, c_muted(), 0);
    lv_obj_set_style_border_color(ovl_pilot, c_amber(), 0);
    pilot_open = true;
    pilot_render();
    lv_obj_remove_flag(ovl_pilot, LV_OBJ_FLAG_HIDDEN);
}

// Input while the pilot dialog is open: turn chooses, press confirms, long press leaves.
static void pilot_on_input(marine_semantic_input_t ev, int32_t val) {
    pilot_opened_ms = snap.now_ms;
    if (ev == MARINE_INPUT_TURN) {
        pilot_idx = (uint8_t)knob_list_wrap(pilot_idx, val > 0 ? 1 : -1, pilot_count);
        pilot_render();
        return;
    }
    if (ev == MARINE_INPUT_LONG_PRESS || ev == MARINE_INPUT_DOUBLE_PRESS) { pilot_close(); return; }
    if (ev != MARINE_INPUT_SINGLE_PRESS) return;
    knob_pilot_line_t l = pilot_lines[pilot_idx];
    pilot_close();
    switch (l) {
    case KNOB_PL_STANDBY: emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_STANDBY, 1, 0); break;
    case KNOB_PL_COMPASS: emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_SET_STATE, KNOB_AP_AUTO, 0); break;
    case KNOB_PL_WIND: emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_SET_STATE, KNOB_AP_WIND, 0); break;
    case KNOB_PL_ROUTE: emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_SET_STATE, KNOB_AP_ROUTE, 0); break;
    case KNOB_PL_TACK_PORT:
    case KNOB_PL_TACK_STBD:
        // A tack asks for the confirm turn towards its side within 5 s, like engage.
        ap_confirm_dir = l == KNOB_PL_TACK_PORT ? -1 : 1;
        ap_confirm_open = true;
        ap_confirm_deadline_ms = snap.now_ms + 5000;
        lv_label_set_text(ap_confirm_lbl_title, ap_confirm_dir < 0 ? "TURN LEFT\nTO TACK\nPORT" : "TURN RIGHT\nTO TACK\nSTARBOARD");
        lv_obj_remove_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        break;
    default: break;
    }
}

static void cancel_transients(void) {
    pilot_close();
    if (ap_confirm_open) {
        ap_confirm_open = false;
        lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
    }
    settings_edit_abort();
    anchor_dialogs_close();
    ap_pending_active = false;
    ap_pending_sent = false;
    dimmer_pending_active = false;
    dimmer_pending_sent = false;
    dimmer_wake_active = false;
    anchor_preview_active = false;
}

// --- Settings screen (#5, ui-design section 3.5). A list of what a skipper changes at the helm. Values come from the snapshot
// (setting[]), changes go out as MARINE_ACT_SETTING_SET and come back through the snapshot once stored. ---
typedef enum { SET_LIST = 0, SET_EDIT, SET_CONFIRM, SET_PAGE, SET_RESET, SET_MODES, SET_MENU } set_view_t;

#define SET_CONFIRM_MS 5000
#define SET_RESET_HOLD_MS 5000

static lv_obj_t *set_lbl_top, *set_lbl_bottom;   // straight lines: list position or item name, hints
static lv_obj_t *set_lbl_prev, *set_lbl_next;      // list: the items before and after the focused one
static lv_obj_t *set_pill, *set_lbl_name, *set_lbl_value;
static lv_obj_t *set_arc;                         // edit (amber), confirm countdown (cyan), reset hold (red)
static lv_obj_t *set_lbl_big, *set_lbl_line1, *set_lbl_line2, *set_lbl_page;

static set_view_t set_view = SET_LIST;
static int set_index = 0;
static int set_group = -1;   // the list shown: -1 the main list, else a page item with a sub-list (MOTOR LIMITS)
static int set_edit_value = 0;
static int set_edit_was = 0;
static uint32_t set_confirm_ms = 0;
static bool set_reset_holding = false;
static lv_obj_t *set_mode_pill[KNOB_MODES_PAGE_COUNT], *set_mode_lbl[KNOB_MODES_PAGE_COUNT];
#define SET_MENU_MAX 4
static lv_obj_t *set_menu_pill, *set_menu_items[SET_MENU_MAX];   // a choice as a pill menu, as in the anchor dialog
static int set_mode_focus = 0;
static uint32_t set_mode_refused_ms = 0;   // the last mode on: the bottom line says so for 2 s

static void set_show(lv_obj_t *o, bool on) {
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_label(lv_obj_t *l, const char *t, lv_color_t col) {
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
    lv_obj_set_style_text_color(l, col, 0);
}

static lv_obj_t *set_make_label(lv_obj_t *scr, const lv_font_t *f, int y) {
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

static lv_obj_t* build_settings_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    set_arc = lv_arc_create(scr);
    lv_obj_set_size(set_arc, 200, 200);
    lv_obj_center(set_arc);
    lv_arc_set_bg_angles(set_arc, 135, 45);
    lv_obj_set_style_arc_width(set_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(set_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(set_arc, lv_color_hex(0x16263E), LV_PART_MAIN);
    lv_obj_remove_style(set_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(set_arc, LV_OBJ_FLAG_CLICKABLE);

    set_lbl_prev = set_make_label(scr, &lv_font_montserrat_14, -52);
    set_lbl_next = set_make_label(scr, &lv_font_montserrat_14, 52);

    set_pill = lv_obj_create(scr);
    lv_obj_set_size(set_pill, 190, 56);
    lv_obj_center(set_pill);
    lv_obj_set_style_radius(set_pill, 28, 0);
    lv_obj_set_style_border_width(set_pill, 2, 0);
    lv_obj_set_style_pad_all(set_pill, 0, 0);
    lv_obj_remove_flag(set_pill, LV_OBJ_FLAG_SCROLLABLE);
    set_lbl_name = lv_label_create(set_pill);
    lv_obj_set_style_text_font(set_lbl_name, &lv_font_montserrat_12, 0);
    lv_obj_align(set_lbl_name, LV_ALIGN_TOP_MID, 0, 7);
    set_lbl_value = lv_label_create(set_pill);
    lv_obj_set_style_text_font(set_lbl_value, &lv_font_montserrat_20, 0);
    lv_obj_align(set_lbl_value, LV_ALIGN_TOP_MID, 0, 24);

    set_lbl_big = set_make_label(scr, &lv_font_montserrat_28, -8);
    set_lbl_line1 = set_make_label(scr, &lv_font_montserrat_14, 26);
    set_lbl_line2 = set_make_label(scr, &lv_font_montserrat_12, 46);
    set_lbl_page = set_make_label(scr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_line_space(set_lbl_page, 4, 0);

    set_menu_pill = lv_obj_create(scr);   // behind the item labels: created first
    lv_obj_set_size(set_menu_pill, 190, 30);
    lv_obj_set_style_radius(set_menu_pill, 15, 0);
    lv_obj_set_style_border_width(set_menu_pill, 0, 0);
    lv_obj_remove_flag(set_menu_pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(set_menu_pill, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < SET_MENU_MAX; i++) {
        set_menu_items[i] = set_make_label(scr, &lv_font_montserrat_14, 0);
        lv_obj_add_flag(set_menu_items[i], LV_OBJ_FLAG_HIDDEN);
    }

    // MODES page: two columns, three rows of pills, filled while the mode is on.
    for (int i = 0; i < KNOB_MODES_PAGE_COUNT; i++) {
        lv_obj_t *p = lv_obj_create(scr);
        lv_obj_set_size(p, 92, 36);
        lv_obj_align(p, LV_ALIGN_CENTER, (i % 2) ? 50 : -50, -44 + (i / 2) * 44);
        lv_obj_set_style_radius(p, 18, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = lv_label_create(p);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_label_set_text(l, knob_modes_page_name[i]);
        lv_obj_center(l);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        set_mode_pill[i] = p;
        set_mode_lbl[i] = l;
    }

    set_lbl_top = set_make_label(scr, &lv_font_montserrat_12, -86);      // about 150 px at y 34
    set_lbl_bottom = set_make_label(scr, &lv_font_montserrat_12, 82);    // in the arc's gap, about 160 px at y 202
    return scr;
}

// Settings left out of the lists: the motor's while the MOTOR mode is off (owner, 2026-11-24).
static uint32_t set_hidden(void) { return snap.motor_mode ? 0 : KNOB_SET_MOTOR_HIDDEN; }

// The MODES page's state from the snapshot: bit i for knob_modes_page[i].
static uint8_t set_modes_mask(void) {
    bool on[KNOB_MODES_PAGE_COUNT] = { snap.ap_mode, snap.lights_mode, snap.anchor_mode, snap.music_mode, snap.motor_mode,
                                       snap.usb_mode };
    uint8_t m = 0;
    for (int i = 0; i < KNOB_MODES_PAGE_COUNT; i++) if (on[i]) m |= (uint8_t)(1u << i);
    return m;
}

// What the list shows as the item's value.
static void set_value_text(int item, char *out, size_t n) {
    if (item == KNOB_SET_SETUP_AP) {
        if (snap.setup_ap_open && snap.setup_ap_left_s) snprintf(out, n, "OPEN %u:%02u", snap.setup_ap_left_s / 60, snap.setup_ap_left_s % 60);
        else snprintf(out, n, "%s", snap.setup_ap_open ? "OPEN" : "CLOSED");
    } else if (item == KNOB_SET_MOTOR_LIMITS) {
        char idle[12];
        knob_setting_format(KNOB_SET_MOTOR_IDLE, snap.setting[KNOB_SET_MOTOR_IDLE], idle, sizeof(idle));
        snprintf(out, n, "%d/%d %% %s", snap.setting[KNOB_SET_MOTOR_AHEAD], snap.setting[KNOB_SET_MOTOR_ASTERN], idle);
    } else if (item == KNOB_SET_MODES) {
        snprintf(out, n, "%d OF %d ON", knob_modes_count(set_modes_mask()), KNOB_MODES_PAGE_COUNT);
    } else if (item == KNOB_SET_DEVICE_INFO || item == KNOB_SET_SIGNALK) {
        snprintf(out, n, "SHOW");
    } else if (item == KNOB_SET_RESET) {
        snprintf(out, n, "HOLD 5 s");
    } else {
        knob_setting_format((knob_setting_t)item, snap.setting[item], out, n);
    }
}

static void settings_refresh(void) {
    if (!set_lbl_top) return;
    // The MOTOR mode was switched off while its settings were shown: back to the main list, off the hidden item.
    if (set_hidden()) {
        if (set_group >= 0 && (set_hidden() & (1u << set_group))) { set_index = set_group; set_group = -1; set_view = SET_LIST; }
        if (set_hidden() & (1u << set_index)) set_index = knob_setting_list_next(set_index, 1, set_group, set_hidden());
    }
    char top[48], bot[48], b[160];
    const knob_setting_desc_t *d = knob_setting_desc((knob_setting_t)set_index);
    bool list = set_view == SET_LIST;
    set_show(set_lbl_prev, list);
    set_show(set_lbl_next, list);
    set_show(set_pill, list);
    set_show(set_arc, set_view == SET_EDIT || set_view == SET_CONFIRM || set_view == SET_RESET);
    set_show(set_lbl_big, set_view == SET_EDIT || set_view == SET_CONFIRM || set_view == SET_RESET);
    set_show(set_lbl_line1, set_view != SET_LIST && set_view != SET_PAGE && set_view != SET_MODES && set_view != SET_MENU);
    set_show(set_lbl_line2, set_view != SET_LIST && set_view != SET_PAGE && set_view != SET_MODES && set_view != SET_MENU);
    set_show(set_lbl_page, set_view == SET_PAGE);
    for (int i = 0; i < KNOB_MODES_PAGE_COUNT; i++) set_show(set_mode_pill[i], set_view == SET_MODES);
    set_show(set_menu_pill, set_view == SET_MENU);
    for (int i = 0; i < SET_MENU_MAX; i++) set_show(set_menu_items[i], set_view == SET_MENU && i < d->n_values);

    switch (set_view) {
    case SET_LIST:
    {
        int pos, cnt;
        knob_setting_list_pos(set_index, set_group, set_hidden(), &pos, &cnt);
        if (set_group < 0) snprintf(top, sizeof(top), "SETTINGS %d/%d", pos, cnt);
        else snprintf(top, sizeof(top), "%s %d/%d", knob_setting_desc((knob_setting_t)set_group)->name, pos, cnt);
    }
        snprintf(bot, sizeof(bot), "%s \xE2\x80\xA2 2x BACK", d->kind == KNOB_KIND_PAGE ? "PRESS OPEN" : "PRESS EDIT");
        set_label(set_lbl_prev, knob_setting_desc((knob_setting_t)knob_setting_list_next(set_index, -1, set_group, set_hidden()))->name, c_muted());
        set_label(set_lbl_next, knob_setting_desc((knob_setting_t)knob_setting_list_next(set_index, 1, set_group, set_hidden()))->name, c_muted());
        lv_obj_set_style_bg_color(set_pill, snap.night_palette ? lv_color_hex(0x050000) : COL_CARD, 0);
        lv_obj_set_style_border_color(set_pill, c_primary(), 0);
        set_label(set_lbl_name, d->name, c_text());
        set_value_text(set_index, b, sizeof(b));
        set_label(set_lbl_value, b, c_primary());
        break;
    case SET_EDIT: {
        snprintf(top, sizeof(top), "%s", d->name);
        snprintf(bot, sizeof(bot), "PRESS STORE \xE2\x80\xA2 2x UNDO");
        knob_setting_format((knob_setting_t)set_index, set_edit_value, b, sizeof(b));
        set_label(set_lbl_big, b, c_amber());
        knob_setting_format((knob_setting_t)set_index, set_edit_was, b, sizeof(b));
        char was[sizeof(b) + 8];
        snprintf(was, sizeof(was), "was %s", b);
        set_label(set_lbl_line1, was, c_muted());
        set_label(set_lbl_line2, "", c_muted());
        int pos = 0, n = d->n_values > 1 ? d->n_values : 2;
        if (d->kind == KNOB_KIND_NUMBER) {
            for (int i = 0; i < d->n_values; i++) if (d->values[i] <= set_edit_value) pos = i;
        } else {
            pos = set_edit_value;
        }
        lv_arc_set_range(set_arc, 0, n - 1);
        lv_arc_set_value(set_arc, pos);
        lv_obj_set_style_arc_color(set_arc, c_amber(), LV_PART_INDICATOR);
        break;
    }
    case SET_CONFIRM: {
        snprintf(top, sizeof(top), "ALLOW CONTROL");
        snprintf(bot, sizeof(bot), "PRESS CANCELS");
        set_label(set_lbl_big, "TURN RIGHT", c_primary());
        set_label(set_lbl_line1, "to allow", c_text());
        set_label(set_lbl_line2, "Autopilot and anchor", c_muted());
        uint32_t el = snap.now_ms - set_confirm_ms;
        lv_arc_set_range(set_arc, 0, SET_CONFIRM_MS);
        lv_arc_set_value(set_arc, el >= SET_CONFIRM_MS ? 0 : (int32_t)(SET_CONFIRM_MS - el));
        lv_obj_set_style_arc_color(set_arc, c_primary(), LV_PART_INDICATOR);
        break;
    }
    case SET_PAGE:
        if (set_index == KNOB_SET_DEVICE_INFO) {
            snprintf(top, sizeof(top), "DEVICE INFO");
            uint32_t up = snap.info_uptime_s;
            if (snap.info_wifi[0]) {
                snprintf(b, sizeof(b), "FW %s\n%s %d dBm\n%s\n%lu KB FREE\nUP %luh %02lum",
                         snap.info_firmware, snap.info_wifi, (int)snap.info_rssi, snap.info_ip, (unsigned long)snap.info_free_kb,
                         (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
            } else {
                snprintf(b, sizeof(b), "FW %s\nNO WI-FI\n%lu KB FREE\nUP %luh %02lum", snap.info_firmware,
                         (unsigned long)snap.info_free_kb, (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60));
            }
        } else {
            snprintf(top, sizeof(top), "SIGNALK");
            snprintf(b, sizeof(b), "%s\nLINK %s\nTOKEN %s\nCONTROL %s", snap.info_server[0] ? snap.info_server : "NO SERVER",
                     snap.info_link, snap.info_token, snap.info_allow_control ? "ALLOWED" : "OFF");
            for (char *q = b; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        }
        snprintf(bot, sizeof(bot), "PRESS BACK");
        set_label(set_lbl_page, b, c_text());
        break;
    case SET_MENU: {
        // All the choices, the focused one in the amber pill, the stored one in the primary colour.
        snprintf(top, sizeof(top), "%s", d->name);
        snprintf(bot, sizeof(bot), "PRESS STORE \xE2\x80\xA2 2x BACK");
        int n = d->n_values < SET_MENU_MAX ? d->n_values : SET_MENU_MAX;
        for (int i = 0; i < n; i++) {
            int y = i * 34 - (n - 1) * 17;
            set_label(set_menu_items[i], d->words[i], i == set_edit_value ? COL_BG : i == set_edit_was ? c_primary() : c_text());
            lv_obj_align(set_menu_items[i], LV_ALIGN_CENTER, 0, y);
            if (i == set_edit_value) lv_obj_align(set_menu_pill, LV_ALIGN_CENTER, 0, y);
        }
        lv_obj_set_style_bg_color(set_menu_pill, c_amber(), 0);
        break;
    }
    case SET_MODES: {
        snprintf(top, sizeof(top), "MODES");
        bool refused = set_mode_refused_ms && snap.now_ms - set_mode_refused_ms < 2000;
        snprintf(bot, sizeof(bot), "%s", refused ? "ONE MODE STAYS ON" : "PRESS SWITCH \xE2\x80\xA2 2x BACK");
        uint8_t mask = set_modes_mask();
        for (int i = 0; i < KNOB_MODES_PAGE_COUNT; i++) {
            bool on = mask & (1u << i), foc = i == set_mode_focus;
            lv_obj_set_style_bg_color(set_mode_pill[i], on ? c_primary() : (snap.night_palette ? lv_color_hex(0x050000) : COL_CARD), 0);
            lv_obj_set_style_bg_opa(set_mode_pill[i], on ? LV_OPA_COVER : LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(set_mode_pill[i], foc ? 3 : 1, 0);
            lv_obj_set_style_border_color(set_mode_pill[i], foc ? (on ? c_text() : c_primary()) : c_muted(), 0);
            lv_obj_set_style_text_color(set_mode_lbl[i], on ? COL_BG : c_muted(), 0);
        }
        break;
    }
    case SET_RESET: {
        snprintf(top, sizeof(top), "RESET SETUP");
        snprintf(bot, sizeof(bot), "PRESS BACK");
        set_label(set_lbl_big, "HOLD 5 s", c_red());
        set_label(set_lbl_line1, "CLEARS WI-FI", c_text());
        set_label(set_lbl_line2, "AND PAIRING", c_text());
        uint32_t held = (set_reset_holding && btn_is_down) ? snap.now_ms - btn_down_ms : 0;
        lv_arc_set_range(set_arc, 0, SET_RESET_HOLD_MS);
        lv_arc_set_value(set_arc, held > SET_RESET_HOLD_MS ? SET_RESET_HOLD_MS : (int32_t)held);
        lv_obj_set_style_arc_color(set_arc, c_red(), LV_PART_INDICATOR);
        break;
    }
    }
    set_label(set_lbl_top, top, set_view == SET_LIST ? c_muted() : c_text());
    set_label(set_lbl_bottom, bot, c_muted());
}

// The brightness shows on the panel while it is edited (SETTING_PREVIEW), it is stored when the edit is left with a press.
static void settings_preview(int value) {
    if (set_index == KNOB_SET_BRIGHTNESS) {
        emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETTING_PREVIEW, KNOB_SET_BRIGHTNESS, value);
    }
}

// An edit that is left without storing (undo, a mode change from elsewhere): the previewed value goes back.
static void settings_edit_abort(void) {
    if (set_view != SET_EDIT && set_view != SET_MENU) return;
    if (set_edit_value != set_edit_was) settings_preview(set_edit_was);
    set_view = SET_LIST;
}

// Night mode, heading, HID profile and demo mode: the choices as a pill menu (owner, 2026-11-25).
static bool set_is_menu(int item) {
    return item == KNOB_SET_NIGHT || item == KNOB_SET_HEADING || item == KNOB_SET_HID_PROFILE || item == KNOB_SET_DEMO;
}

// Opening the settings always starts in the list, never in an edit left behind.
static void settings_enter(void) {
    settings_edit_abort();
    set_view = SET_LIST;
    if (set_group >= 0) { set_index = set_group; set_group = -1; }   // always the main list
    set_reset_holding = false;
}

// The settings screen keeps a double press for itself while it edits, confirms or shows a page (cancel or back). In the
// list a double press opens the mode ring like everywhere.
static bool settings_takes_double(void) {
    return active_mode == MARINE_MODE_SETTINGS && (set_view != SET_LIST || set_group >= 0);   // a sub-list: back
}

static void settings_on_input(marine_semantic_input_t ev, int32_t val) {
    const knob_setting_desc_t *d = knob_setting_desc((knob_setting_t)set_index);
    switch (set_view) {
    case SET_LIST:
        if (ev == MARINE_INPUT_TURN) {
            set_index = knob_setting_list_next(set_index, val, set_group, set_hidden());
        } else if (ev == MARINE_INPUT_DOUBLE_PRESS && set_group >= 0) {
            set_index = set_group;   // back to the main list, on the page item
            set_group = -1;
        } else if (ev == MARINE_INPUT_SINGLE_PRESS && knob_setting_has_sub((knob_setting_t)set_index)) {
            set_group = set_index;   // open the sub-list (MOTOR LIMITS)
            set_index = knob_setting_list_first(set_group, set_hidden());
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            if (set_index == KNOB_SET_ALLOW_CONTROL) {
                if (snap.setting[KNOB_SET_ALLOW_CONTROL]) {
                    emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETTING_SET, KNOB_SET_ALLOW_CONTROL, 0); // on to off: one press
                } else {
                    set_view = SET_CONFIRM;
                    set_confirm_ms = snap.now_ms;
                }
            } else if (set_index == KNOB_SET_SETUP_AP) {
                emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETUP_AP, snap.setup_ap_open ? 0 : 1, 0);
            } else if (set_index == KNOB_SET_RESET) {
                set_view = SET_RESET;
                set_reset_holding = false;
            } else if (set_index == KNOB_SET_MODES) {
                set_view = SET_MODES;
                set_mode_focus = 0;
                set_mode_refused_ms = 0;
            } else if (d->kind == KNOB_KIND_PAGE) {
                set_view = SET_PAGE;
            } else {
                set_view = set_is_menu(set_index) ? SET_MENU : SET_EDIT;
                set_edit_value = set_edit_was = snap.setting[set_index];
            }
        }
        break;
    case SET_EDIT:
    case SET_MENU:
        if (ev == MARINE_INPUT_TURN) {
            int v = knob_setting_step((knob_setting_t)set_index, set_edit_value, val);
            if (v != set_edit_value) settings_preview(v);
            set_edit_value = v;
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            if (set_edit_value != set_edit_was) {
                emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETTING_SET, set_index, set_edit_value);
            }
            set_view = SET_LIST;
        } else if (ev == MARINE_INPUT_DOUBLE_PRESS) {
            settings_edit_abort();   // cancel, nothing stored
        }
        break;
    case SET_CONFIRM: {
        knob_confirm_ev_t ce = ev == MARINE_INPUT_TURN ? KNOB_CONFIRM_EV_TURN : KNOB_CONFIRM_EV_PRESS;
        knob_confirm_t r = knob_confirm_step(set_confirm_ms, snap.now_ms, SET_CONFIRM_MS, ce, (int)val);
        if (r == KNOB_CONFIRM_DONE) {
            emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETTING_SET, KNOB_SET_ALLOW_CONTROL, 1);
            set_view = SET_LIST;
        } else if (r == KNOB_CONFIRM_CANCELLED) {
            set_view = SET_LIST;
        }
        break;
    }
    case SET_PAGE:
        if (ev == MARINE_INPUT_SINGLE_PRESS || ev == MARINE_INPUT_DOUBLE_PRESS) set_view = SET_LIST;
        break;
    case SET_RESET:
        if (ev == MARINE_INPUT_SINGLE_PRESS || ev == MARINE_INPUT_DOUBLE_PRESS) set_view = SET_LIST;
        break;
    case SET_MODES:
        if (ev == MARINE_INPUT_TURN) {
            set_mode_focus = knob_list_wrap(set_mode_focus, val, KNOB_MODES_PAGE_COUNT);
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            uint8_t mask = set_modes_mask();
            if (knob_modes_toggle_ok(mask, set_mode_focus)) {
                bool on = !(mask & (1u << set_mode_focus));
                emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SETTING_SET, KNOB_SET_MODES, (set_mode_focus << 8) | (on ? 1 : 0));
                set_mode_refused_ms = 0;
            } else {
                set_mode_refused_ms = snap.now_ms ? snap.now_ms : 1;   // the last one on stays on
            }
        } else if (ev == MARINE_INPUT_DOUBLE_PRESS) {
            set_view = SET_LIST;
        }
        break;
    }
    settings_refresh();
}

// Timers of the settings screen: the confirm turn runs out, the reset hold fills. Called from marine_ui_tick().
static void settings_tick(void) {
    if (active_mode != MARINE_MODE_SETTINGS || snap.system_screen != MARINE_SCREEN_OPERATIONAL || mode_ring_open) return;
    if (set_view == SET_CONFIRM) {
        if (knob_confirm_step(set_confirm_ms, snap.now_ms, SET_CONFIRM_MS, KNOB_CONFIRM_EV_TICK, 0) == KNOB_CONFIRM_CANCELLED) {
            set_view = SET_LIST;
        }
        settings_refresh();
    } else if (set_view == SET_MODES && set_mode_refused_ms) {
        if (snap.now_ms - set_mode_refused_ms >= 2000) set_mode_refused_ms = 0;
        settings_refresh();
    } else if (set_view == SET_RESET) {
        if (btn_is_down && !set_reset_holding) set_reset_holding = true;
        if (!btn_is_down) set_reset_holding = false;
        if (set_reset_holding && snap.now_ms - btn_down_ms >= SET_RESET_HOLD_MS) {
            set_reset_holding = false;
            set_view = SET_LIST;
            emit_action(MARINE_MODE_SETTINGS, MARINE_ACT_SYS_FACTORY_RESET, 1, 0);   // reset setup: Wi-Fi and pairing
        }
        settings_refresh();
    }
}

// --- Music (#6, ui-design section 3.6): signalk-jukebox from the helm. Turn changes the selected target's volume by 2 %, sent
// after a 0.4 s pause; press plays or pauses; long press steps to the next target. Touch: previous left, next right, play or
// pause at the bottom, the target name at the top for the next target. The outer arc only shows the volume. ---
#define VOL_COMMIT_MS 400

static lv_obj_t *vol_arc, *vol_lbl_target, *vol_lbl_track, *vol_lbl_artist, *vol_lbl_pct, *vol_lbl_state;
static lv_obj_t *vol_btn_prev, *vol_btn_next, *vol_btn_play;
static lv_obj_t *vol_ic_prev, *vol_ic_next, *vol_ic_play;
static uint8_t *vol_ic_prev_buf, *vol_ic_next_buf, *vol_ic_play_buf;
static uint32_t vol_ic_prev_col = 0xFFFFFFFFu, vol_ic_next_col = 0xFFFFFFFFu, vol_ic_play_col = 0xFFFFFFFFu;
static int vol_ic_play_shown = -1;   // ICON_PLAY or ICON_PAUSE on the bottom button

static uint8_t vol_sel = 0;          // selected target: 0 master, then the zones
static bool vol_pending_active = false, vol_pending_sent = false;
static int vol_pending_pct = 0;
static uint32_t vol_pending_deadline_ms = 0;

static const marine_vol_target_t *vol_target(void) {
    if (vol_sel >= snap.vol_target_count) vol_sel = 0;
    return &snap.vol_targets[vol_sel];
}

static lv_obj_t *vol_make_button(lv_obj_t *scr, int dx, int dy, int size, lv_obj_t **icon, uint8_t **buf) {
    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_set_size(b, size, size);
    lv_obj_align(b, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    *icon = icon_canvas_create(b, size - 16, buf);
    lv_obj_center(*icon);
    return b;
}

static lv_obj_t* build_music_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    vol_arc = lv_arc_create(scr);
    lv_obj_set_size(vol_arc, 222, 222);
    lv_obj_center(vol_arc);
    lv_arc_set_bg_angles(vol_arc, 135, 45);
    lv_arc_set_range(vol_arc, 0, 100);
    lv_obj_set_style_arc_width(vol_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(vol_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(vol_arc, lv_color_hex(0x16263E), LV_PART_MAIN);
    lv_obj_remove_style(vol_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(vol_arc, LV_OBJ_FLAG_CLICKABLE);

    vol_lbl_target = anc_make_label(scr, &lv_font_montserrat_14, -74);
    // Track and artist come from outside (any language): the fonts with Latin-1, Latin Extended-A, Cyrillic and Greek.
    vol_lbl_track = anc_make_label(scr, &knob_font_14, -52);
    lv_label_set_long_mode(vol_lbl_track, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(vol_lbl_track, 150);
    lv_obj_set_style_text_align(vol_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(vol_lbl_track, LV_ALIGN_CENTER, 0, -52);
    vol_lbl_artist = anc_make_label(scr, &knob_font_14, -36);
    lv_label_set_long_mode(vol_lbl_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(vol_lbl_artist, 140);
    lv_obj_set_style_text_align(vol_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(vol_lbl_artist, LV_ALIGN_CENTER, 0, -36);
    vol_lbl_pct = anc_make_label(scr, &lv_font_montserrat_44, 4);
    vol_lbl_state = anc_make_label(scr, &lv_font_montserrat_12, 36);

    vol_btn_prev = vol_make_button(scr, -78, 4, 40, &vol_ic_prev, &vol_ic_prev_buf);
    vol_btn_next = vol_make_button(scr, 78, 4, 40, &vol_ic_next, &vol_ic_next_buf);
    vol_btn_play = vol_make_button(scr, 0, 72, 44, &vol_ic_play, &vol_ic_play_buf);
    return scr;
}

static void music_refresh(void) {
    if (!vol_arc) return;
    const marine_vol_target_t *t = vol_target();
    bool stale = current_stale() != STALE_NONE;
    bool playing = snap.play_state == MARINE_PLAY_PLAYING;
    int pct = vol_pending_active ? vol_pending_pct : t->pct;
    char b[24];

    lv_arc_set_value(vol_arc, stale ? 0 : pct);
    lv_obj_set_style_arc_color(vol_arc, vol_pending_active ? c_amber() : (playing ? c_primary() : lv_color_hex(0x3A4A63)),
                               LV_PART_INDICATOR);

    snprintf(b, sizeof(b), "%s " LV_SYMBOL_DOWN, t->name);   // the marker says the name can be tapped
    anc_set_line(vol_lbl_target, b, c_text());
    anc_set_line(vol_lbl_track, snap.track[0] ? snap.track : (snap.play_state == MARINE_PLAY_STOPPED ? "STOPPED" : ""), c_text());
    anc_set_line(vol_lbl_artist, snap.artist, c_muted());
    if (stale) snprintf(b, sizeof(b), "---");
    else snprintf(b, sizeof(b), "%d", pct);
    anc_set_line(vol_lbl_pct, b, vol_pending_active ? c_amber() : c_text());
    // The line under the volume: the playback position while it is known (0:53, PAUSED 0:53), else the state.
    char st[24] = "";
    bool paused = snap.play_state == MARINE_PLAY_PAUSED;
    if (snap.track_pos_s >= 0 && snap.play_state != MARINE_PLAY_STOPPED) {
        snprintf(st, sizeof(st), "%s%d:%02d", paused ? "PAUSED " : "", (int)(snap.track_pos_s / 60), (int)(snap.track_pos_s % 60));
    } else if (paused) {
        snprintf(st, sizeof(st), "PAUSED");
    }
    anc_set_line(vol_lbl_state, !t->connected ? "ZONE OFFLINE" : st, !t->connected ? c_amber() : c_muted());

    lv_obj_t *btns[3] = { vol_btn_prev, vol_btn_next, vol_btn_play };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(btns[i], snap.night_palette ? lv_color_hex(0x050000) : COL_CARD, 0);
        lv_obj_set_style_border_color(btns[i], snap.night_palette ? COL_NIGHT_DIM : lv_color_hex(0x284166), 0);
    }
    icon_recolor(vol_ic_prev, ICON_PREV, 24, c_text(), &vol_ic_prev_col);
    icon_recolor(vol_ic_next, ICON_NEXT, 24, c_text(), &vol_ic_next_col);
    int want = playing ? ICON_PAUSE : ICON_PLAY;
    if (want != vol_ic_play_shown) {
        vol_ic_play_shown = want;
        vol_ic_play_col = 0xFFFFFFFFu;   // a different icon: redraw
    }
    icon_recolor(vol_ic_play, (icon_id_t)want, 28, c_primary(), &vol_ic_play_col);
}

static void music_play_pause(void) {
    emit_action(MARINE_MODE_MUSIC, MARINE_ACT_VOL_CONTROL,
                snap.play_state == MARINE_PLAY_PLAYING ? MARINE_VOL_PAUSE : MARINE_VOL_PLAY, 0);
}

static void music_next_target(void) {
    vol_pending_active = false;
    vol_sel = (uint8_t)knob_list_wrap(vol_sel, 1, snap.vol_target_count ? snap.vol_target_count : 1);
}

static void music_on_input(marine_semantic_input_t ev, int32_t val) {
    const marine_vol_target_t *t = vol_target();
    if (ev == MARINE_INPUT_TURN) {
        if (!t->connected) return;                    // ZONE OFFLINE ignores the turn
        int base = vol_pending_active ? vol_pending_pct : t->pct;
        vol_pending_pct = knob_vol_step(base, (int)val);
        vol_pending_active = true;
        vol_pending_sent = false;
        vol_pending_deadline_ms = snap.now_ms + VOL_COMMIT_MS;
    } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
        music_play_pause();
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        music_next_target();
    }
    music_refresh();
}

// The 0.4 s pause commit, and the pending value held until the server's value matches. Called from marine_ui_tick().
static void music_tick(void) {
    if (!vol_pending_active) return;
    if (!vol_pending_sent && snap.now_ms >= vol_pending_deadline_ms) {
        vol_pending_sent = true;
        emit_action(MARINE_MODE_MUSIC, MARINE_ACT_VOL_SET, vol_pending_pct, vol_sel);
    }
    if (vol_pending_sent && (vol_target()->pct == vol_pending_pct ||
                             snap.now_ms >= vol_pending_deadline_ms + MARINE_PENDING_CONFIRM_TIMEOUT)) {
        vol_pending_active = false;
        vol_pending_sent = false;
    }
    if (active_mode == MARINE_MODE_MUSIC) music_refresh();
}

// Touch on the music screen (the centre dead zone is handled by the caller): only the player buttons and the target name.
static void music_touch(int16_t x, int16_t y) {
    int dx = x - 120, dy = y - 120;
    if (dx * dx + dy * dy > 100 * 100) return;   // the outer arc only shows the volume
    if (dy < -50 && dx > -60 && dx < 60) music_next_target();
    else if (dx < -55 && dy > -30 && dy < 40) emit_action(MARINE_MODE_MUSIC, MARINE_ACT_VOL_CONTROL, MARINE_VOL_PREVIOUS, 0);
    else if (dx > 55 && dy > -30 && dy < 40) emit_action(MARINE_MODE_MUSIC, MARINE_ACT_VOL_CONTROL, MARINE_VOL_NEXT, 0);
    else if (dy > 45 && dx > -35 && dx < 35) music_play_pause();
    music_refresh();
}

static uint8_t vol_sel_peek(void) { return vol_sel < snap.vol_target_count ? vol_sel : 0; }

// --- Motor (#8, docs/plans/issue-8-motor-mode.md): the throttle. The rules (arming, the stop on neutral, limits, every reason
// to fall back to neutral) live in knob_motor in knob_logic, run by the firmware; this screen shows its state and passes the
// clicks and presses on. Neutral is at 12 o'clock: ahead fills clockwise in green, astern counter-clockwise in amber, 150 degrees
// each way for 100 %. A thick arc for the knob's throttle, a dot for what the motor reports. ---
#define MOTOR_SWEEP_DEG 150
#define MOTOR_SHOW_WHY_MS 5000   // a refusal or disarm reason stays on top this long (a fault stays until the next arm)
static lv_obj_t *mot_arc_ahead, *mot_arc_astern, *mot_arc_confirm, *mot_dot_rep, *mot_dot_lim[2];
static lv_obj_t *mot_lbl_top, *mot_lbl_gear, *mot_lbl_set, *mot_lbl_rpm, *mot_lbl_data, *mot_lbl_hint;
static lv_obj_t *ovl_update;      // UPDATING FIRMWARE, while an update is installed
static lv_obj_t *ovl_motor_tag;   // F 40 % on every other screen while the motor is out of neutral

static lv_color_t c_green(void) { return snap.night_palette ? COL_NIGHT_RED : COL_GREEN; }

static lv_obj_t *mot_make_arc(lv_obj_t *scr, int size, int w, int a0, int a1) {
    lv_obj_t *a = lv_arc_create(scr);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_bg_angles(a, a0, a1);
    lv_arc_set_range(a, 0, 100);
    lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x16263E), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    return a;
}

static lv_obj_t *mot_make_dot(lv_obj_t *scr, int d) {
    lv_obj_t *o = lv_obj_create(scr);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

// A dot on a circle of radius r at a throttle in percent (0 at 12 o'clock, +100 at 150 degrees clockwise).
static void mot_place(lv_obj_t *o, int d, float r, float pct) {
    float deg = 270.0f + pct * (float)MOTOR_SWEEP_DEG / 100.0f;
    float rad = deg * (float)(M_PI / 180.0);
    lv_obj_set_pos(o, (int32_t)(120.0f + r * cosf(rad)) - d / 2, (int32_t)(120.0f + r * sinf(rad)) - d / 2);
}

static lv_obj_t* build_motor_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 240, 240);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    mot_arc_ahead = mot_make_arc(scr, 222, 10, 270, 270 + MOTOR_SWEEP_DEG - 360);
    mot_arc_astern = mot_make_arc(scr, 222, 10, 270 - MOTOR_SWEEP_DEG, 270);
    lv_arc_set_mode(mot_arc_astern, LV_ARC_MODE_REVERSE);   // fills from 12 o'clock counter-clockwise
    mot_arc_confirm = mot_make_arc(scr, 196, 4, 0, 360);    // the confirm countdown
    lv_obj_set_style_arc_opa(mot_arc_confirm, LV_OPA_TRANSP, LV_PART_MAIN);
    mot_dot_rep = mot_make_dot(scr, 10);
    for (int i = 0; i < 2; i++) mot_dot_lim[i] = mot_make_dot(scr, 6);

    mot_lbl_top = anc_make_label(scr, &lv_font_montserrat_20, -80);
    lv_label_set_long_mode(mot_lbl_top, LV_LABEL_LONG_MODE_DOTS);   // about 170 px at y 40
    lv_obj_set_width(mot_lbl_top, 170);
    lv_obj_set_style_text_align(mot_lbl_top, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mot_lbl_top, LV_ALIGN_CENTER, 0, -80);
    mot_lbl_gear = anc_make_label(scr, &lv_font_montserrat_44, -26);
    mot_lbl_set = anc_make_label(scr, &lv_font_montserrat_20, 14);
    mot_lbl_rpm = anc_make_label(scr, &lv_font_montserrat_14, 40);
    mot_lbl_data = anc_make_label(scr, &lv_font_montserrat_12, 60);
    mot_lbl_hint = anc_make_label(scr, &lv_font_montserrat_12, 82);
    lv_label_set_long_mode(mot_lbl_hint, LV_LABEL_LONG_MODE_DOTS);  // in the arc's gap, about 160 px at y 202
    lv_obj_set_width(mot_lbl_hint, 160);
    lv_obj_set_style_text_align(mot_lbl_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(mot_lbl_hint, LV_ALIGN_CENTER, 0, 82);
    return scr;
}

static bool mot_fresh(uint32_t ms) { return ms != 0 && snap.now_ms - ms <= 5000; }

static void motor_refresh(void) {
    if (!mot_arc_ahead) return;
    char b[48];
    int set = snap.motor_set_pct;
    bool armed = snap.motor_state == 2, confirm = snap.motor_state == 1;

    lv_arc_set_value(mot_arc_ahead, set > 0 ? set : 0);
    lv_arc_set_value(mot_arc_astern, set < 0 ? -set : 0);
    lv_obj_set_style_arc_color(mot_arc_ahead, armed ? c_green() : c_muted(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(mot_arc_astern, armed ? c_amber() : c_muted(), LV_PART_INDICATOR);

    if (confirm) {
        uint32_t el = snap.now_ms - snap.motor_confirm_ms;
        int left = el >= 5000 ? 0 : (int)((5000 - el) / 50);   // 100 .. 0
        lv_arc_set_value(mot_arc_confirm, left);
        lv_obj_set_style_arc_color(mot_arc_confirm, c_green(), LV_PART_INDICATOR);
        lv_obj_remove_flag(mot_arc_confirm, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mot_arc_confirm, LV_OBJ_FLAG_HIDDEN);
    }

    if (mot_fresh(snap.motor_reported_ms)) {
        float r = snap.motor_reported_pct;
        if (r > 100) r = 100;
        if (r < -100) r = -100;
        mot_place(mot_dot_rep, 10, 100.0f, r);
        lv_obj_set_style_bg_color(mot_dot_rep, c_text(), 0);
        lv_obj_remove_flag(mot_dot_rep, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(mot_dot_rep, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 2; i++) {
        if (snap.motor_limit_lifted) { lv_obj_add_flag(mot_dot_lim[i], LV_OBJ_FLAG_HIDDEN); continue; }
        mot_place(mot_dot_lim[i], 6, 111.0f, i == 0 ? (float)snap.motor_max_ahead_pct : -(float)snap.motor_max_astern_pct);
        lv_obj_set_style_bg_color(mot_dot_lim[i], c_amber(), 0);
        lv_obj_remove_flag(mot_dot_lim[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Top: the state, or for a while why the knob refused or let go.
    bool fault = !armed && !confirm && snap.motor_why == 5;   // KNOB_MOTOR_WHY_FAULT stays until the next arm
    bool show_why = !armed && !confirm && snap.motor_why_ms && snap.now_ms - snap.motor_why_ms < MOTOR_SHOW_WHY_MS;
    static const char *const whys[] = { "", "CONTROL IS OFF", "NO LINK", "NO MOTOR DATA", "SET NEUTRAL FIRST", "MOTOR FAULT",
                                        "IDLE, DISARMED", "DISARMED", "CANCELLED" };
    if (fault) anc_set_line(mot_lbl_top, "MOTOR FAULT", c_red());
    else if (show_why && snap.motor_why < sizeof(whys) / sizeof(whys[0]) && whys[snap.motor_why][0])
        anc_set_line(mot_lbl_top, whys[snap.motor_why], snap.motor_why == 7 ? c_muted() : c_amber());
    else if (armed) anc_set_line(mot_lbl_top, "ARMED", c_green());
    else if (confirm) anc_set_line(mot_lbl_top, "ARM MOTOR?", c_text());
    else anc_set_line(mot_lbl_top, "MOTOR", c_muted());

    // Centre: the gear the motor reports, large.
    bool gear_ok = mot_fresh(snap.motor_gear_ms);
    int g = gear_ok ? snap.motor_gear : 0;
    const char *gw = g == 1 ? "F" : g == 2 ? "N" : g == 3 ? "R" : g == 4 ? "!" : "---";
    anc_set_line(mot_lbl_gear, gw, g == 1 ? c_green() : g == 3 ? c_amber() : g == 4 ? c_red() : g == 2 ? c_text() : c_muted());

    if (armed) {
        if (set == 0) snprintf(b, sizeof(b), "NEUTRAL");
        else snprintf(b, sizeof(b), "%s %d %%", set > 0 ? "AHEAD" : "ASTERN", set > 0 ? set : -set);
        anc_set_line(mot_lbl_set, b, set > 0 ? c_green() : set < 0 ? c_amber() : c_text());
    } else {
        anc_set_line(mot_lbl_set, "", c_muted());
    }
    if (mot_fresh(snap.motor_rpm_ms)) snprintf(b, sizeof(b), "%d RPM", (int)lroundf(snap.motor_rpm));
    else b[0] = '\0';
    anc_set_line(mot_lbl_rpm, b, c_text());
    b[0] = '\0';
    size_t n = 0;
    if (mot_fresh(snap.motor_torque_ms)) n += (size_t)snprintf(b + n, sizeof(b) - n, "TORQUE %d %%", (int)lroundf(snap.motor_torque_pct));
    if (mot_fresh(snap.motor_power_ms) && n < sizeof(b)) {
        float w = snap.motor_power_w;
        if (w >= 1000.0f || w <= -1000.0f) snprintf(b + n, sizeof(b) - n, "%s%.1f kW", n ? " \xE2\x80\xA2 " : "", w / 1000.0f);
        else snprintf(b + n, sizeof(b) - n, "%s%d W", n ? " \xE2\x80\xA2 " : "", (int)lroundf(w));
    }
    anc_set_line(mot_lbl_data, b, c_muted());

    // Bottom: what the knob does next.
    if (armed && snap.motor_limit_warning) {
        snprintf(b, sizeof(b), "LIMIT %d %% \xE2\x80\xA2 2x: ALLOW MORE", set >= 0 ? snap.motor_max_ahead_pct : snap.motor_max_astern_pct);
        anc_set_line(mot_lbl_hint, b, c_amber());
    } else if (armed) {
        anc_set_line(mot_lbl_hint, set != 0 ? "HOLD: NEUTRAL" : "HOLD: DISARM", c_muted());
    } else if (confirm) {
        anc_set_line(mot_lbl_hint, "TURN RIGHT TO ARM", c_text());
    } else {
        anc_set_line(mot_lbl_hint, "HOLD: ARM", c_muted());
    }
}

// Every click goes to the firmware one by one, at once: knob_motor measures the time between them for the step size.
static void motor_on_input(marine_semantic_input_t ev, int32_t val) {
    if (ev == MARINE_INPUT_TURN) {
        int dir = val < 0 ? -1 : 1, n = val < 0 ? -val : val;
        for (int i = 0; i < n; i++) emit_action(MARINE_MODE_MOTOR, MARINE_ACT_MOTOR_TURN, dir, 0);
    } else if (ev == MARINE_INPUT_LONG_PRESS) {
        emit_action(MARINE_MODE_MOTOR, MARINE_ACT_MOTOR_LONG, 1, 0);
    }   // a single press does nothing here: it only counts after the 300 ms double-press wait, too slow for neutral
    motor_refresh();
}

// The tag on the other screens while the motor runs: F 40 % in green, R 15 % in amber. Not on the alarm screen.
static void motor_tag_update(void) {
    if (!ovl_motor_tag) return;
    int set = snap.motor_set_pct;
    bool show = set != 0 && active_mode != MARINE_MODE_MOTOR && !is_full_alarm_blocking() &&
                snap.system_screen == MARINE_SCREEN_OPERATIONAL;
    if (!show) { lv_obj_add_flag(ovl_motor_tag, LV_OBJ_FLAG_HIDDEN); return; }
    char b[12];
    snprintf(b, sizeof(b), "%c %d %%", set > 0 ? 'F' : 'R', set > 0 ? set : -set);
    anc_set_line(ovl_motor_tag, b, set > 0 ? c_green() : c_amber());
    lv_obj_remove_flag(ovl_motor_tag, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ovl_motor_tag);
}

// --- CONTROL ON notice: allow control was switched on from the web page or the API (ui-design section 4) ---
static void build_control_notice(void) {
    ovl_ctl_ring = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_ctl_ring, 236, 236);
    lv_obj_center(ovl_ctl_ring);
    lv_obj_set_style_radius(ovl_ctl_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ovl_ctl_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ovl_ctl_ring, 3, 0);
    lv_obj_remove_flag(ovl_ctl_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_ctl_ring, LV_OBJ_FLAG_HIDDEN);

    ovl_ctl_pill = lv_obj_create(lv_layer_top());
    lv_obj_set_size(ovl_ctl_pill, 160, 44);
    lv_obj_align(ovl_ctl_pill, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_radius(ovl_ctl_pill, 22, 0);
    lv_obj_set_style_border_width(ovl_ctl_pill, 2, 0);
    lv_obj_set_style_pad_all(ovl_ctl_pill, 0, 0);
    lv_obj_remove_flag(ovl_ctl_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ovl_ctl_pill, LV_OBJ_FLAG_HIDDEN);

    ctl_lbl_title = lv_label_create(ovl_ctl_pill);
    lv_obj_set_style_text_font(ctl_lbl_title, &lv_font_montserrat_14, 0);
    lv_label_set_text(ctl_lbl_title, "CONTROL ON");
    lv_obj_align(ctl_lbl_title, LV_ALIGN_TOP_MID, 0, 6);

    ctl_lbl_sub = lv_label_create(ovl_ctl_pill);
    lv_obj_set_style_text_font(ctl_lbl_sub, &lv_font_montserrat_12, 0);
    lv_label_set_text(ctl_lbl_sub, "SET OVER THE NETWORK");
    lv_obj_align(ctl_lbl_sub, LV_ALIGN_TOP_MID, 0, 24);
}

static void control_notice_hide(void) {
    if (!ctl_notice_on) return;
    ctl_notice_on = false;
    lv_obj_add_flag(ovl_ctl_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ovl_ctl_pill, LV_OBJ_FLAG_HIDDEN);
}

static void control_notice_show(void) {
    if (is_full_alarm_blocking()) return;   // the alarm screen wins, the notice is not queued
    lv_color_t accent = snap.night_palette ? COL_NIGHT_RED : COL_GREEN;
    lv_obj_set_style_border_color(ovl_ctl_ring, accent, 0);
    lv_obj_set_style_border_color(ovl_ctl_pill, accent, 0);
    lv_obj_set_style_bg_color(ovl_ctl_pill, snap.night_palette ? lv_color_hex(0x050000) : COL_CARD, 0);
    lv_obj_set_style_text_color(ctl_lbl_title, accent, 0);
    lv_obj_set_style_text_color(ctl_lbl_sub, c_muted(), 0);
    lv_obj_remove_flag(ovl_ctl_ring, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ovl_ctl_pill, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ovl_ctl_ring);
    lv_obj_move_foreground(ovl_ctl_pill);
    ctl_notice_on = true;
    ctl_notice_until_ms = snap.now_ms + CTL_NOTICE_MS;
    // Wake the screen, so the notice is seen from across the cockpit.
    last_input_ms = snap.now_ms;
    if (current_backlight_pct != 100) {
        current_backlight_pct = 100;
        emit_action(active_mode, MARINE_ACT_POWER_SET_BACKLIGHT, 100, 0);
    }
}

// --- Stale overlay, evaluated every tick and on every snapshot (a silent link never calls apply_snapshot) ---
static void update_stale_overlay(void) {
    stale_state_t st = (snap.system_screen == MARINE_SCREEN_OPERATIONAL) ? current_stale() : STALE_NONE;
    const char *reason = (st == STALE_ESSENTIAL) ? essential_stale_reason() : NULL;
    // No data since boot: the link is still coming up, not lost. Shows CONNECTING and the boot phase instead of an age.
    bool connecting = (st == STALE_LINK) && snap.last_data_ms == 0;
    static char last_phase[sizeof(snap.boot_phase)];
    uint32_t age_s = 0;
    if (st == STALE_LINK && !connecting) {
        age_s = (snap.now_ms >= snap.last_data_ms) ? ((snap.now_ms - snap.last_data_ms) / 1000U) : 0U;
    }

    uint32_t sig = display_stale_bits()
                 | ((uint32_t)st << 8)
                 | ((uint32_t)active_mode << 12)
                 | (mode_ring_open ? (1u << 16) : 0u)
                 | (is_full_alarm_blocking() ? (1u << 17) : 0u);
    bool changed = (sig != last_stale_sig) || (reason != last_stale_reason);
    bool age_changed = (st == STALE_LINK) && (age_s != last_stale_age_s);
    if (connecting && strcmp(last_phase, snap.boot_phase) != 0) age_changed = true;
    if (connecting) memcpy(last_phase, snap.boot_phase, sizeof(last_phase));
    else last_phase[0] = '\0';
    if (!changed && !age_changed) return;
    last_stale_sig = sig;
    last_stale_reason = reason;
    last_stale_age_s = age_s;

    if (changed && st != STALE_NONE) cancel_transients();

    // The overlay never covers the mode ring (the user can always leave) or the full-screen alarm.
    bool show = (st != STALE_NONE) && !mode_ring_open && !is_full_alarm_blocking();
    if (show) {
        if (connecting) {
            lv_label_set_text(stale_lbl_title, "CONNECTING");
            lv_label_set_text(stale_lbl_age, snap.boot_phase);
            lv_label_set_text(stale_lbl_sub, "CONTROLS OFF");
        } else if (st == STALE_LINK) {
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

// The alarm icon fades between full and a quarter, once a second.
static void alarm_pulse_cb(void *o, int32_t v) { lv_obj_set_style_text_opa(o, (lv_opa_t)v, 0); }
static void alarm_pulse(bool on) {
    if (on == alm_pulsing) return;
    alm_pulsing = on;
    if (!on) { lv_anim_delete(alm_icon, alarm_pulse_cb); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, alm_icon);
    lv_anim_set_exec_cb(&a, alarm_pulse_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, 64);
    lv_anim_set_duration(&a, 500);
    lv_anim_set_reverse_duration(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
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

    // A press while the CONTROL ON notice shows only dismisses it. Turns and other presses work as usual.
    if (ctl_notice_on && ev == MARINE_INPUT_SINGLE_PRESS) {
        control_notice_hide();
        return;
    }

    if (snap.system_screen != MARINE_SCREEN_OPERATIONAL) {
        return;
    }

    // Stale does NOT block the mode ring (below): the user can always leave a stale mode. Only the mode's own
    // controls are gated further down. HID mode is never stale (it needs the USB host, not the SignalK link).

    if (ev == MARINE_INPUT_DOUBLE_PRESS && !mode_ring_open && settings_takes_double()) {
        settings_on_input(ev, 0);   // cancel the edit or leave the page, the ring opens from the list only
        return;
    }

    if (ev == MARINE_INPUT_DOUBLE_PRESS && !mode_ring_open && active_mode == MARINE_MODE_MOTOR && snap.motor_state == 2 &&
        snap.motor_limit_warning) {
        emit_action(MARINE_MODE_MOTOR, MARINE_ACT_MOTOR_ACK, 1, 0);   // lifts both limits, raises nothing
        return;
    }

    if (ev == MARINE_INPUT_DOUBLE_PRESS) {
        anchor_dialogs_close();
        if (mode_ring_open) close_mode_ring(false);
        else open_mode_ring();
        return;
    }

    if (mode_ring_open) {
        if (ev == MARINE_INPUT_TURN) {
            ring_opened_ms = snap.now_ms;
            for (int i = 1; i <= MARINE_MODE_COUNT; i++) {
                uint8_t next = (uint8_t)knob_list_wrap(ring_selected_slot, val * i, MARINE_MODE_COUNT);
                if (!ring_slot_visible(next)) continue;
                ring_selected_slot = next;
                break;
            }
            refresh_mode_ring();
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            close_mode_ring(true);
        }
        return;
    }

    // The motor's inputs always reach knob_motor, also with the link or the motor's data gone: it refuses or falls back to
    // neutral by itself, and a long press for neutral must never be swallowed here.
    if (active_mode == MARINE_MODE_MOTOR) {
        motor_on_input(ev, val);
        return;
    }

    // The pilot dialog, and opening it with a long press while engaged, work while the pilot's data is stale: STANDBY must
    // always be reachable (ui-design section 3.1). Stale data limits the dialog to STANDBY and CANCEL.
    if (active_mode == MARINE_MODE_AUTOPILOT && pilot_open) {
        pilot_on_input(ev, val);
        return;
    }
    if (active_mode == MARINE_MODE_AUTOPILOT && snap.ap_engaged && ev == MARINE_INPUT_LONG_PRESS && !ap_confirm_open &&
        current_stale() != STALE_NONE) {
        pilot_open_dialog(true);
        return;
    }

    if (current_stale() != STALE_NONE) {
        return; // the active mode's own controls are off while its data is stale
    }

    if (ap_confirm_open && ap_confirm_dir != 0) {   // tack: one click towards the tack side
        knob_confirm_ev_t ce = ev == MARINE_INPUT_TURN ? KNOB_CONFIRM_EV_TURN : KNOB_CONFIRM_EV_PRESS;
        knob_confirm_t r = knob_confirm_step(ap_confirm_deadline_ms - 5000, snap.now_ms, 5000, ce, (int)val * ap_confirm_dir);
        if (r == KNOB_CONFIRM_DONE) emit_action(MARINE_MODE_AUTOPILOT, MARINE_ACT_AP_TACK, ap_confirm_dir, 0);
        if (r != KNOB_CONFIRM_WAITING) {
            ap_confirm_open = false;
            lv_obj_add_flag(ovl_ap_confirm, LV_OBJ_FLAG_HIDDEN);
        }
        return;
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

    if (anc_pend != ANC_PEND_NONE) {
        // The pending screen only waits. A press dismisses it (the call is sent already), a refusal goes with any input.
        if (ev == MARINE_INPUT_SINGLE_PRESS || (anc_pend == ANC_PEND_REFUSED && ev != MARINE_INPUT_DOUBLE_PRESS)) anchor_pending_close();
        return;
    }

    if (anchor_menu_open) {
        if (ev == MARINE_INPUT_TURN) {
            anchor_menu_opened_ms = snap.now_ms;
            anchor_menu_idx = (uint8_t)((anchor_menu_idx + (val > 0 ? 1 : anc_menu_count - 1)) % anc_menu_count);
            anchor_menu_render();
        } else if (ev == MARINE_INPUT_SINGLE_PRESS) {
            anchor_menu_choose();
        } else if (ev == MARINE_INPUT_LONG_PRESS) {
            anchor_dialogs_close();   // a long press again leaves the dialog without choosing
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
    build_control_notice();
    ovl_demo_tag = lv_label_create(lv_layer_top());
    lv_label_set_text(ovl_demo_tag, "DEMO");
    lv_obj_set_style_text_font(ovl_demo_tag, &lv_font_montserrat_12, 0);
    lv_obj_align(ovl_demo_tag, LV_ALIGN_CENTER, 0, 110);   // below the bottom lines, inside the round edge
    lv_obj_add_flag(ovl_demo_tag, LV_OBJ_FLAG_HIDDEN);
    ovl_motor_tag = lv_label_create(lv_layer_top());
    lv_label_set_text(ovl_motor_tag, "");
    lv_obj_set_style_text_font(ovl_motor_tag, &lv_font_montserrat_14, 0);
    lv_obj_align(ovl_motor_tag, LV_ALIGN_CENTER, 0, 95);   // above the DEMO tag
    lv_obj_add_flag(ovl_motor_tag, LV_OBJ_FLAG_HIDDEN);
    // A small opaque pill in the middle. The controls stay usable, an alarm covers it.
    ovl_update = lv_label_create(lv_layer_top());
    lv_label_set_text(ovl_update, "UPDATING FIRMWARE");
    lv_obj_set_style_text_font(ovl_update, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_opa(ovl_update, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ovl_update, 2, 0);
    lv_obj_set_style_radius(ovl_update, 18, 0);
    lv_obj_set_style_pad_hor(ovl_update, 14, 0);
    lv_obj_set_style_pad_ver(ovl_update, 8, 0);
    lv_obj_center(ovl_update);
    lv_obj_add_flag(ovl_update, LV_OBJ_FLAG_HIDDEN);
    // Above every dialog and tag, below the DEMO tag (the alarm is created early, the dialogs after it).
    lv_obj_move_to_index(ovl_alarm, lv_obj_get_index(ovl_demo_tag) - 1);
    dfx_create();   // last: the demo overlay sits above everything else on the top layer
    lv_screen_load(scr_modes[active_mode]);
    mode_registry[active_mode].refresh();
}

// --- Automatic demo overlay (#10, docs/plans/issue-10-auto-demo.md): the simulated inputs made visible. On the top layer,
// created last so it sits above every screen, the ring and the alarm takeover. Only the automatic demo calls it. ---
static lv_obj_t *dfx_turn_arc, *dfx_turn_tip, *dfx_turn_count;
static lv_obj_t *dfx_pulse[2], *dfx_long_arc, *dfx_touch_dot, *dfx_touch_ring;
static lv_obj_t *dfx_cap_box, *dfx_cap_lbl, *dfx_count_bg, *dfx_count_lbl;
static uint32_t dfx_turn_ms, dfx_touch_ms;
static bool dfx_turn_on, dfx_touch_on;

static void dfx_anim_size(void *o, int32_t v) {
    lv_obj_set_size(o, v, v);
    lv_obj_center(o);
}
static void dfx_anim_size_at(void *o, int32_t v) {       // keeps the centre where the touch was (stored in user data)
    intptr_t xy = (intptr_t)lv_obj_get_user_data(o);
    lv_obj_set_size(o, v, v);
    lv_obj_set_pos(o, (int32_t)(xy >> 16) - v / 2, (int32_t)(xy & 0xFFFF) - v / 2);
}
static void dfx_anim_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void dfx_anim_arc(void *o, int32_t v) { lv_arc_set_value(o, v); }
static void dfx_anim_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static lv_obj_t *dfx_ring_obj(int w) {
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, w, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

// A ring grows from the centre and fades out: the knob's push.
static void dfx_pulse_start(lv_obj_t *o, uint32_t delay_ms) {
    lv_anim_delete(o, NULL);
    lv_obj_set_style_border_color(o, c_primary(), 0);
    lv_obj_set_style_opa(o, LV_OPA_COVER, 0);
    dfx_anim_size(o, 34);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_values(&a, 34, 150);
    lv_anim_set_exec_cb(&a, dfx_anim_size);
    lv_anim_start(&a);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_exec_cb(&a, dfx_anim_opa);
    lv_anim_set_completed_cb(&a, dfx_anim_done);
    lv_anim_start(&a);
}

static void dfx_create(void) {
    dfx_turn_arc = lv_arc_create(lv_layer_top());
    lv_obj_set_size(dfx_turn_arc, 236, 236);
    lv_obj_center(dfx_turn_arc);
    lv_arc_set_bg_angles(dfx_turn_arc, 0, 0);
    lv_obj_set_style_arc_opa(dfx_turn_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(dfx_turn_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(dfx_turn_arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(dfx_turn_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(dfx_turn_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dfx_turn_arc, LV_OBJ_FLAG_HIDDEN);
    dfx_turn_tip = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(dfx_turn_tip, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(dfx_turn_tip, LV_OBJ_FLAG_HIDDEN);
    dfx_turn_count = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(dfx_turn_count, &lv_font_montserrat_20, 0);
    lv_obj_add_flag(dfx_turn_count, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; i++) dfx_pulse[i] = dfx_ring_obj(3);

    dfx_long_arc = lv_arc_create(lv_layer_top());
    lv_obj_set_size(dfx_long_arc, 96, 96);
    lv_obj_center(dfx_long_arc);
    lv_arc_set_rotation(dfx_long_arc, 270);
    lv_arc_set_bg_angles(dfx_long_arc, 0, 360);
    lv_arc_set_range(dfx_long_arc, 0, 100);
    lv_obj_set_style_arc_width(dfx_long_arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(dfx_long_arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(dfx_long_arc, LV_OPA_30, LV_PART_MAIN);
    lv_obj_remove_style(dfx_long_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(dfx_long_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dfx_long_arc, LV_OBJ_FLAG_HIDDEN);

    dfx_touch_ring = dfx_ring_obj(3);
    dfx_touch_dot = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dfx_touch_dot, 34, 34);
    lv_obj_set_style_radius(dfx_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dfx_touch_dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dfx_touch_dot, LV_OPA_50, 0);
    lv_obj_set_style_border_width(dfx_touch_dot, 0, 0);
    lv_obj_remove_flag(dfx_touch_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(dfx_touch_dot, LV_OBJ_FLAG_HIDDEN);

    dfx_cap_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dfx_cap_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(dfx_cap_box, LV_ALIGN_CENTER, 0, -101);
    lv_obj_set_style_radius(dfx_cap_box, 10, 0);
    lv_obj_set_style_bg_color(dfx_cap_box, COL_BG, 0);
    lv_obj_set_style_bg_opa(dfx_cap_box, LV_OPA_80, 0);
    lv_obj_set_style_border_width(dfx_cap_box, 1, 0);
    lv_obj_set_style_pad_hor(dfx_cap_box, 8, 0);
    lv_obj_set_style_pad_ver(dfx_cap_box, 2, 0);
    lv_obj_remove_flag(dfx_cap_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    dfx_cap_lbl = lv_label_create(dfx_cap_box);
    lv_obj_set_style_text_font(dfx_cap_lbl, &knob_font_14, 0);
    lv_obj_add_flag(dfx_cap_box, LV_OBJ_FLAG_HIDDEN);

    dfx_count_bg = lv_obj_create(lv_layer_top());
    lv_obj_set_size(dfx_count_bg, 240, 240);
    lv_obj_center(dfx_count_bg);
    lv_obj_set_style_bg_color(dfx_count_bg, COL_BG, 0);
    lv_obj_set_style_bg_opa(dfx_count_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dfx_count_bg, 0, 0);
    lv_obj_set_style_radius(dfx_count_bg, 0, 0);
    lv_obj_remove_flag(dfx_count_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    dfx_count_lbl = lv_label_create(dfx_count_bg);
    lv_obj_set_style_text_font(dfx_count_lbl, &lv_font_montserrat_44, 0);
    lv_obj_set_style_transform_scale(dfx_count_lbl, 512, 0);   // twice the 44 px font
    lv_obj_center(dfx_count_lbl);
    lv_obj_add_flag(dfx_count_bg, LV_OBJ_FLAG_HIDDEN);
}

void marine_ui_demo_fx(marine_demo_fx_t fx, int a, int b) {
    if (!dfx_turn_arc) return;
    lv_color_t col = c_primary();
    switch (fx) {
    case MARINE_DEMO_FX_TURN: {
        // A short arc on the rim on the side of the turn (right for clockwise), a chevron at its end and the click count.
        bool cw = a >= 0;
        lv_arc_set_angles(dfx_turn_arc, cw ? 330 : 150, cw ? 30 : 210);
        lv_obj_set_style_arc_color(dfx_turn_arc, col, LV_PART_INDICATOR);
        lv_label_set_text(dfx_turn_tip, LV_SYMBOL_DOWN);
        lv_obj_set_style_text_color(dfx_turn_tip, col, 0);
        lv_obj_align(dfx_turn_tip, LV_ALIGN_CENTER, cw ? 100 : -100, 58);
        char buf[16];
        snprintf(buf, sizeof(buf), "%s%d", cw ? "+" : "-", b);
        lv_label_set_text(dfx_turn_count, buf);
        lv_obj_set_style_text_color(dfx_turn_count, col, 0);
        lv_obj_align(dfx_turn_count, LV_ALIGN_CENTER, cw ? 86 : -86, 0);
        lv_obj_remove_flag(dfx_turn_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(dfx_turn_tip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(dfx_turn_count, LV_OBJ_FLAG_HIDDEN);
        dfx_turn_on = true;
        dfx_turn_ms = snap.now_ms;
        break;
    }
    case MARINE_DEMO_FX_PRESS: dfx_pulse_start(dfx_pulse[0], 0); break;
    case MARINE_DEMO_FX_DOUBLE: dfx_pulse_start(dfx_pulse[0], 0); dfx_pulse_start(dfx_pulse[1], 200); break;
    case MARINE_DEMO_FX_LONG: {
        lv_anim_delete(dfx_long_arc, NULL);
        lv_obj_set_style_arc_color(dfx_long_arc, col, LV_PART_MAIN);
        lv_obj_set_style_arc_color(dfx_long_arc, col, LV_PART_INDICATOR);
        lv_obj_set_style_opa(dfx_long_arc, LV_OPA_COVER, 0);
        lv_arc_set_value(dfx_long_arc, 0);
        lv_obj_remove_flag(dfx_long_arc, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, dfx_long_arc);
        lv_anim_set_duration(&an, 750);              // the UI's long press time
        lv_anim_set_values(&an, 0, 100);
        lv_anim_set_exec_cb(&an, dfx_anim_arc);
        lv_anim_start(&an);
        lv_anim_set_delay(&an, 750);
        lv_anim_set_duration(&an, 300);
        lv_anim_set_values(&an, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&an, dfx_anim_opa);
        lv_anim_set_completed_cb(&an, dfx_anim_done);
        lv_anim_start(&an);
        dfx_pulse_start(dfx_pulse[0], 750);          // the moment it fires
        break;
    }
    case MARINE_DEMO_FX_TOUCH: {
        lv_obj_set_pos(dfx_touch_dot, a - 17, b - 17);
        lv_obj_remove_flag(dfx_touch_dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_user_data(dfx_touch_ring, (void *)(intptr_t)((a << 16) | (b & 0xFFFF)));
        lv_anim_delete(dfx_touch_ring, NULL);
        lv_obj_set_style_border_color(dfx_touch_ring, lv_color_white(), 0);
        lv_obj_set_style_opa(dfx_touch_ring, LV_OPA_COVER, 0);
        lv_obj_remove_flag(dfx_touch_ring, LV_OBJ_FLAG_HIDDEN);
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, dfx_touch_ring);
        lv_anim_set_duration(&an, 500);
        lv_anim_set_values(&an, 34, 80);
        lv_anim_set_exec_cb(&an, dfx_anim_size_at);
        lv_anim_start(&an);
        lv_anim_set_values(&an, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&an, dfx_anim_opa);
        lv_anim_set_completed_cb(&an, dfx_anim_done);
        lv_anim_start(&an);
        dfx_touch_on = true;
        dfx_touch_ms = snap.now_ms;
        break;
    }
    case MARINE_DEMO_FX_COUNT:
        if (a <= 0) {
            lv_obj_add_flag(dfx_count_bg, LV_OBJ_FLAG_HIDDEN);
        } else {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d", a);
            lv_label_set_text(dfx_count_lbl, buf);
            lv_obj_set_style_text_color(dfx_count_lbl, col, 0);
            lv_obj_center(dfx_count_lbl);
            lv_obj_remove_flag(dfx_count_bg, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    case MARINE_DEMO_FX_CLEAR:
    default: {
        lv_obj_t *all[] = { dfx_turn_arc, dfx_turn_tip, dfx_turn_count, dfx_pulse[0], dfx_pulse[1], dfx_long_arc,
                            dfx_touch_dot, dfx_touch_ring, dfx_count_bg, dfx_cap_box };
        for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            lv_anim_delete(all[i], NULL);
            lv_obj_add_flag(all[i], LV_OBJ_FLAG_HIDDEN);
        }
        dfx_turn_on = dfx_touch_on = false;
        break;
    }
    }
}

// The turn arc fades 0.6 s after the last click, the fingertip 0.5 s after the touch. From marine_ui_tick().
static void dfx_tick(void) {
    if (dfx_turn_on && snap.now_ms - dfx_turn_ms >= 600) {
        dfx_turn_on = false;
        lv_obj_add_flag(dfx_turn_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dfx_turn_tip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dfx_turn_count, LV_OBJ_FLAG_HIDDEN);
    }
    if (dfx_touch_on && snap.now_ms - dfx_touch_ms >= 500) {
        dfx_touch_on = false;
        lv_obj_add_flag(dfx_touch_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

void marine_ui_demo_caption(const char *text) {
    if (!dfx_cap_box) return;
    if (!text || !text[0]) {
        lv_obj_add_flag(dfx_cap_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(dfx_cap_lbl, text);
    lv_obj_set_style_text_color(dfx_cap_lbl, c_primary(), 0);
    lv_obj_set_style_border_color(dfx_cap_box, c_primary(), 0);
    lv_obj_remove_flag(dfx_cap_box, LV_OBJ_FLAG_HIDDEN);
}

bool marine_ui_ring_is_open(void) { return mode_ring_open; }

marine_ui_mode_t marine_ui_ring_focus(void) {
    return mode_ring_open ? ring_slot_modes[ring_selected_slot] : active_mode;
}

int marine_ui_settings_index(void) {
    if (active_mode != MARINE_MODE_SETTINGS || mode_ring_open || set_view != SET_LIST || set_group >= 0) return -1;
    return set_index;
}

void marine_ui_load_demo_snapshot(marine_ui_snapshot_t *out_snap) {    if (!out_snap) return;
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
    out_snap->anchor_dropped = true;
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
    out_snap->track_pos_s = -1;
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
        // A knob push from a dark screen first reaches the glass: that touch woke the screen, the push only belongs to it.
        btn_swallow = touch_woke && (snap.now_ms - touch_wake_ms) < TOUCH_WAKE_SWALLOW_MS;
        touch_woke = false;
        if (btn_swallow) btn_long_fired = true;   // no long press and no click from this press
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
        if (btn_swallow) {
            btn_swallow = false;
            return;
        }
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
    // Called on every poll while a finger is down: act on the touch-down only.
    if (!pressed) {
        touch_is_down = false;
        return;
    }
    if (touch_is_down) return;
    touch_is_down = true;

    // A dark screen (backlight 0) wakes on a touch anywhere, the dead zone included, and the touch does nothing else
    // (ui-design section 6). From the dim level any touch wakes too, and one outside the dead zone also acts.
    if (current_backlight_pct == 0) {
        note_user_activity(false);
        touch_woke = true;
        touch_wake_ms = snap.now_ms;
        return;
    }
    int32_t dx = (int32_t)x - 120;
    int32_t dy = (int32_t)y - 120;
    if ((dx * dx + dy * dy) < (TOUCH_DEADZONE_PX * TOUCH_DEADZONE_PX)) {
        note_user_activity(false);   // wakes from dim and keeps the screen awake, never acts
        return;
    }

    // Every touch wakes from the dim level and keeps the screen awake, in any mode. The backlight is above 0 here, so the
    // touch may also act.
    note_user_activity(false);

    // The music screen is a small music player (ui-design section 3.6): harmless, so touch may drive it.
    if (snap.system_screen == MARINE_SCREEN_OPERATIONAL && !is_full_alarm_blocking() && !mode_ring_open &&
        active_mode == MARINE_MODE_MUSIC && current_stale() == STALE_NONE) {
        music_touch(x, y);
        return;
    }

    // Restricted to a single harmless role: step to next Dimmer group when in Dimmer mode
    if (snap.system_screen == MARINE_SCREEN_OPERATIONAL &&
        !is_full_alarm_blocking() &&
        !mode_ring_open &&
        current_stale() == STALE_NONE &&
        active_mode == MARINE_MODE_DIMMER &&
        !snap.dimmer_use_rgbw_variant) {
        dimmer_on_input(MARINE_INPUT_SINGLE_PRESS, 0);
    }
}

void marine_ui_apply_snapshot(const marine_ui_snapshot_t *snapshot) {
    if (!snapshot) return;
    uint32_t prev_now_ms = snap.now_ms;
    snap = *snapshot;
    if (snap.now_ms < prev_now_ms) snap.now_ms = prev_now_ms;   // keep the tick clock monotonic

    if (snap.demo) {
        lv_obj_set_style_text_color(ovl_demo_tag, snap.night_palette ? COL_NIGHT_RED : COL_PURPLE, 0);
        lv_obj_remove_flag(ovl_demo_tag, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ovl_demo_tag);
    } else {
        lv_obj_add_flag(ovl_demo_tag, LV_OBJ_FLAG_HIDDEN);
    }

    motor_tag_update();

    if (snap.updating && !is_full_alarm_blocking()) {
        lv_obj_set_style_bg_color(ovl_update, snap.night_palette ? lv_color_hex(0x050000) : COL_CARD, 0);
        lv_obj_set_style_border_color(ovl_update, c_amber(), 0);
        lv_obj_set_style_text_color(ovl_update, c_amber(), 0);
        lv_obj_remove_flag(ovl_update, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ovl_update);
    } else {
        lv_obj_add_flag(ovl_update, LV_OBJ_FLAG_HIDDEN);
    }

    static bool ring_night;
    if (mode_ring_open && ring_night != snap.night_palette) refresh_mode_ring();   // the palette changed under the open ring
    ring_night = snap.night_palette;

    if (snap.control_on_seq != ctl_last_seq) {
        ctl_last_seq = snap.control_on_seq;
        if (snap.control_on_seq != 0 && snap.system_screen == MARINE_SCREEN_OPERATIONAL) control_notice_show();
    }

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

        bool is_drag = (snap.active_alarm == MARINE_ALARM_ANCHOR_DRAG);
        char dist_b[32];
        if (is_drag) snprintf(dist_b, sizeof(dist_b), "%d m OF %u m", (int)roundf(snap.anchor_dist_m), snap.anchor_radius_m);
        else snprintf(dist_b, sizeof(dist_b), "NO GPS FIX");
        lv_obj_set_style_border_color(ovl_alarm, c_red(), 0);
        anc_set_line(alm_lbl_title, is_drag ? "DRAG ALARM" : "GPS LOST", c_red());
        anc_set_line(alm_icon, is_drag ? LV_SYMBOL_WARNING : LV_SYMBOL_GPS, c_red());
        anc_set_line(alm_lbl_dist, dist_b, c_text());
        anc_set_line(alm_lbl_ack, snap.alarm_acknowledged ? "ACKNOWLEDGED" : "PRESS TO ACK", snap.alarm_acknowledged ? c_amber() : c_muted());
        alarm_pulse(true);
        update_stale_overlay();   // hides the stale overlay behind the full-screen alarm
        return;
    }
    lv_obj_add_flag(ovl_alarm, LV_OBJ_FLAG_HIDDEN);
    alarm_pulse(false);

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
        // The status lines and the pairing text are shown per screen below
        lv_obj_remove_flag(sys_lbl_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(sys_lbl_bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_pair_big, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_pair_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sys_pair_name, LV_OBJ_FLAG_HIDDEN);

        if (snap.system_screen == MARINE_SCREEN_BOOT) {
            sys_set_state("HELM DIAL", c_primary());
            lv_obj_add_flag(sys_lbl_bottom, LV_OBJ_FLAG_HIDDEN);   // the splash shows its phase in its own status pill
            lv_obj_remove_flag(sys_arc_prog, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_value(sys_arc_prog, snap.boot_progress_pct);
            lv_label_set_text(sys_lbl_title, "HELM DIAL");
            lv_label_set_text(sys_lbl_line1, "STARTING");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_WIFI_PORTAL) {
            sys_set_state("JOIN WI-FI", c_amber());
            anc_set_line(sys_lbl_bottom, "SETUP ONLY", c_muted());
            lv_obj_remove_flag(sys_pill_box, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sys_lbl_title, snap.setup_ap_ssid[0] ? snap.setup_ap_ssid : "HelmDial-Setup");
            lv_label_set_text(sys_lbl_line1, "THEN OPEN THE");
            lv_label_set_text(sys_lbl_line2, "SETUP PAGE");
        } else if (snap.system_screen == MARINE_SCREEN_CONNECTING) {
            sys_set_state("CONNECTING", c_primary());
            anc_set_line(sys_lbl_bottom, "PLEASE WAIT", c_muted());
            sys_set_steps(true, snap.connect_step);   // drawn marks, no glyph fonts
            lv_label_set_text(sys_lbl_title, "");
            lv_label_set_text(sys_lbl_line1, "");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_PAIRING) {
            // Large text in the middle, no status lines on this screen.
            lv_obj_add_flag(sys_lbl_state, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(sys_lbl_bottom, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(sys_lbl_title, "");
            lv_label_set_text(sys_lbl_line1, "");
            lv_label_set_text(sys_lbl_line2, "");
            lv_obj_remove_flag(sys_pair_big, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sys_pair_hint, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(sys_pair_name, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(sys_pair_big, c_amber(), 0);
            lv_obj_set_style_text_color(sys_pair_hint, c_text(), 0);
            lv_obj_set_style_text_color(sys_pair_name, c_muted(), 0);
            lv_label_set_text(sys_pair_name, snap.device_name);
        } else if (snap.system_screen == MARINE_SCREEN_CANNOT_CONNECT) {
            sys_set_state("NO SERVER", c_red());
            anc_set_line(sys_lbl_bottom, "RETRYING", c_muted());
            lv_label_set_text(sys_lbl_title, "NO SERVER");
            lv_label_set_text(sys_lbl_line1, "WI-FI OK");
            lv_label_set_text(sys_lbl_line2, "");
        } else if (snap.system_screen == MARINE_SCREEN_RESET_SETUP) {
            sys_set_state("RESET", c_red());
            anc_set_line(sys_lbl_bottom, "RELEASE TO CANCEL", c_muted());
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
    settings_tick();
    music_tick();
    dfx_tick();
    if (active_mode == MARINE_MODE_MOTOR) motor_refresh();   // the confirm countdown and the reason timeouts
    if (pilot_open && (now_ms - pilot_opened_ms >= 5000 || !snap.ap_engaged || active_mode != MARINE_MODE_AUTOPILOT ||
                       is_full_alarm_blocking())) {
        pilot_close();   // 5 s without input, or the pilot went to standby by itself, leaves the dialog
    }

    if (ctl_notice_on && (now_ms >= ctl_notice_until_ms || is_full_alarm_blocking() ||
                          snap.system_screen != MARINE_SCREEN_OPERATIONAL)) {
        control_notice_hide();
    }

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
    anchor_pending_update();

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
    if (snap.motor_set_pct != 0) target_bl = 100;   // the motor is running: no dim, no off (#8)
    else if (idle_off_ms != 0 && idle_ms > idle_off_ms && !keep_awake) target_bl = 0;
    else if (idle_dim_ms != 0 && idle_ms > idle_dim_ms) target_bl = idle_dim_pct;   // 0 = never dim

    if (target_bl != current_backlight_pct) {
        current_backlight_pct = target_bl;
        emit_action(active_mode, MARINE_ACT_POWER_SET_BACKLIGHT, target_bl, 0);
    }
}

void marine_ui_set_mode(marine_ui_mode_t mode) {
    if ((int)mode < 0 || mode >= MARINE_MODE_COUNT || mode == active_mode) return;
    if (mode_ring_open) close_mode_ring(false);
    active_mode = mode;
    anchor_preview_active = false;
    cancel_transients();
    // Under a system screen the mode screen is loaded when the snapshot leaves it (marine_ui_apply_snapshot).
    if (snap.system_screen == MARINE_SCREEN_OPERATIONAL) {
        lv_screen_load(scr_modes[active_mode]);
        mode_registry[active_mode].refresh();
    }
    update_stale_overlay();
}

marine_ui_mode_t marine_ui_get_active_mode(void) {
    return active_mode;
}

void marine_ui_set_power(uint32_t dim_after_ms, uint8_t dim_pct, uint32_t off_after_ms) {
    idle_dim_ms = dim_after_ms;
    idle_dim_pct = dim_pct > 100 ? 100 : dim_pct;
    idle_off_ms = off_after_ms;
}

uint8_t marine_ui_get_selected_light(void) { return dimmer_selected_group; }
uint8_t marine_ui_get_selected_vol_target(void) { return vol_sel; }
