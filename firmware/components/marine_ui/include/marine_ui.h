#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Primary operational modes (fixed slots on Mode Ring:
 *        0 = AUTO (Top), 1 = LIGHT (Right), 2 = ANCHOR (Bottom), 3 = USB (Left)).
 */
typedef enum {
    MARINE_MODE_AUTOPILOT = 0,
    MARINE_MODE_DIMMER,
    MARINE_MODE_ANCHOR_WATCH,
    MARINE_MODE_USB_HID,
    MARINE_MODE_COUNT
} marine_ui_mode_t;

typedef enum {
    MARINE_SCREEN_OPERATIONAL = 0,
    MARINE_SCREEN_BOOT,
    MARINE_SCREEN_WIFI_PORTAL,
    MARINE_SCREEN_CONNECTING,
    MARINE_SCREEN_PAIRING,
    MARINE_SCREEN_CANNOT_CONNECT,
    MARINE_SCREEN_RESET_SETUP
} marine_system_screen_t;

typedef enum {
    MARINE_ALARM_NONE = 0,
    MARINE_ALARM_ANCHOR_DRAG,
    MARINE_ALARM_GPS_LOST
} marine_alarm_type_t;

typedef enum {
    HID_PROFILE_CHARTPLOTTER = 0,
    HID_PROFILE_MEDIA_AUDIO,
    HID_PROFILE_NAV_KEYS,
    HID_PROFILE_COUNT
} usb_hid_profile_t;

/**
 * @brief Semantic input events emitted by the hardware button/encoder classifier.
 *        Modes never consume raw keys or raw GPIO edges.
 */
typedef enum {
    MARINE_INPUT_TURN = 0,      // value = signed step delta (+1 CW, -1 CCW)
    MARINE_INPUT_SINGLE_PRESS,  // classified after ~300 ms release window
    MARINE_INPUT_DOUBLE_PRESS,  // opens/closes Mode Ring
    MARINE_INPUT_LONG_PRESS     // >= 750 ms hold
} marine_semantic_input_t;

/**
 * @brief Outgoing action callback: UI never mutates server state directly.
 */
typedef enum {
    MARINE_ACT_AP_SET_CTS = 0,      // value = target degrees (0..359)
    MARINE_ACT_AP_ENGAGE,           // value = 1
    MARINE_ACT_AP_STANDBY,          // value = 1
    MARINE_ACT_ANCHOR_SET_RADIUS,   // value = radius in meters (10..150)
    MARINE_ACT_ANCHOR_ARM_HERE,     // value = 1
    MARINE_ACT_ANCHOR_MOVE_DROP,    // value = 1
    MARINE_ACT_ANCHOR_DISARM,       // value = 1
    MARINE_ACT_ALARM_ACK,           // value = marine_alarm_type_t
    MARINE_ACT_DIMMER_SET_LEVEL,    // value = 0..100 % (active group in meta)
    MARINE_ACT_DIMMER_TOGGLE_POWER, // value = group_index
    MARINE_ACT_DIMMER_SET_PRESET,   // value = preset_index (0..6 optional RGBW variant)
    MARINE_ACT_HID_TURN,            // value = signed step delta
    MARINE_ACT_HID_PRESS,           // value = 1
    MARINE_ACT_HID_NEXT_PROFILE,    // value = 1
    MARINE_ACT_SYS_FACTORY_RESET,   // value = 1 (fires after full 5 s hold)
    MARINE_ACT_POWER_SET_BACKLIGHT, // value = 0, 10, or 100 %
    MARINE_ACT_ALARM_LED_PULSE,     // value = 1 flash LEDs fast (alarm arrival or type change), 0 = stop (alarm cleared)
    MARINE_ACT_ALARM_LED_SLOW_PULSE // value = 1 slow red pulse (alarm acknowledged, still active), 0 = stop
} marine_ui_action_type_t;

typedef struct {
    marine_ui_mode_t mode;
    marine_ui_action_type_t action;
    int32_t value;
    int32_t meta;
} marine_ui_action_t;

typedef void (*marine_ui_action_cb_t)(const marine_ui_action_t *act, void *user_ctx);

#define MARINE_MAX_DIMMER_GROUPS 6
#define MARINE_RGBW_PRESET_COUNT 7

typedef struct {
    char name[16];
    bool on;
    uint8_t level_pct; // 0..100. For an on/off channel (onoff_only) the firmware reports 100 when on and 0 when off.
    bool onoff_only;   // a relay, not a dimmer: turn right = on, turn left = off, no level
} marine_dimmer_group_t;

/**
 * @brief Read-only state snapshot supplied by the firmware (SignalK subscriptions).
 *
 * Staleness is decided per value. Every value that can go stale has a pair:
 *   `<name>_updated_ms`  time the value last arrived, same clock as `now_ms` (0 = never received)
 *   `<name>_timeout_ms`  how long it stays valid. The firmware fills it from the SignalK `meta.timeout` of the key
 *                        (seconds, converted to ms) or from a per-path default. 0 = never stale by age
 *                        (event-driven keys, `timeout: 0`). The UI computes the age itself.
 *
 * Display-only values (rudder, depth, voltage, GPS fix): when stale only that value is masked with `---`.
 * Essential values (AP state, heading, target; anchor data; dimmer levels): when stale the mode shows the stale overlay
 * with a reason and ignores its own controls. The mode ring still opens, and HID mode ignores SignalK staleness.
 * Stream down (`signalk_connected == false`) shows LINK LOST.
 */
typedef struct {
    uint32_t now_ms;
    marine_system_screen_t system_screen;
    bool night_palette;

    // Visual effects toggles (for SPI frame-time profiling)
    bool fx_glass_enabled;
    bool fx_glow_enabled;
    bool fx_ring_pulse_enabled;   // slow opacity pulse of the acknowledged-alarm ring (off = steady ring)

    // Alarm Acknowledge behavior option:
    // true  = dismiss full-screen alarm on ack, return to active mode with persistent red ring & badge
    // false = keep full-screen alarm overlay active until server clears alarm
    bool alarm_ack_return_with_badge;

    // Link health
    bool signalk_connected;
    uint32_t last_data_ms;

    // Telemetry (display-only)
    float depth_m;
    uint32_t depth_updated_ms;
    uint32_t depth_timeout_ms;
    float voltage_v;
    uint32_t voltage_updated_ms;
    uint32_t voltage_timeout_ms;
    bool gps_3d_fix;
    uint32_t gps_updated_ms;
    uint32_t gps_timeout_ms;

    // Autopilot state (essential: ap_engaged, heading, cts; display-only: rudder)
    bool ap_engaged;
    uint32_t ap_state_updated_ms;
    uint32_t ap_state_timeout_ms;
    float heading_deg;
    uint32_t heading_updated_ms;
    uint32_t heading_timeout_ms;
    float cts_deg;
    uint32_t cts_updated_ms;
    uint32_t cts_timeout_ms;
    float rudder_deg;
    uint32_t rudder_updated_ms;
    uint32_t rudder_timeout_ms;

    // Anchor Watch & Alarms (essential: anchor distance, bearing and radius)
    bool anchor_armed;
    uint16_t anchor_radius_m;
    float anchor_dist_m;
    float anchor_bearing_deg;
    uint32_t anchor_updated_ms;
    uint32_t anchor_timeout_ms;
    marine_alarm_type_t active_alarm;
    bool alarm_acknowledged;

    // Dimmer Groups (Primary) + Optional 7-Preset RGBW Variant Flag
    uint32_t dimmer_updated_ms;   // essential: group levels (0 timeout = never stale by age)
    uint32_t dimmer_timeout_ms;
    bool dimmer_use_rgbw_variant;
    uint8_t dimmer_group_count;
    marine_dimmer_group_t dimmer_groups[MARINE_MAX_DIMMER_GROUPS];
    uint8_t rgbw_preset_idx; // 0..6

    // USB-HID Host State
    bool usb_host_enumerated;
    usb_hid_profile_t hid_profile;

    // Boot / Provisioning / Pairing State
    char device_name[24];
    char setup_ap_ssid[32];
    uint8_t boot_progress_pct;   // 0..100
    uint8_t connect_step;        // 0 = Wi-Fi, 1 = Server, 2 = SignalK Link
    char boot_phase[16];         // text in the boot splash status bar, upper case, for example "WI-FI" or "SIGNALK"
    char heading_ref;            // 'T' for true or 'M' for magnetic, shown after the heading degrees. 0 = no label
} marine_ui_snapshot_t;

/**
 * @brief Initialize LVGL 9.3 UI tree with clean zero defaults (firmware owns initial state).
 *        Caller must hold the custom LVGL port mutex around all LVGL calls.
 */
void marine_ui_init(marine_ui_action_cb_t action_cb, void *user_ctx);

/**
 * @brief Optional helper to populate demo values for standalone bench testing.
 */
void marine_ui_load_demo_snapshot(marine_ui_snapshot_t *out_snap);

/**
 * @brief Push a fresh SignalK state snapshot to refresh the active screen.
 */
void marine_ui_apply_snapshot(const marine_ui_snapshot_t *snapshot);

/**
 * @brief Feed raw hardware encoder clicks (+1/-1) and button press/release edges
 *        into the semantic classifier (300 ms single, double, 750 ms long).
 */
void marine_ui_feed_encoder_step(int8_t step_delta);
void marine_ui_feed_button_edge(bool pressed);

/**
 * @brief Feed touch coordinates from CST816D. Restricted to a single harmless role
 *        (stepping to the next dimmer group on outer-ring tap in Dimmer mode).
 *        Never triggers Long Press or Double Press.
 */
void marine_ui_feed_touch(int16_t x, int16_t y, bool pressed);

/**
 * @brief Periodic tick (call every ~50 ms under port lock).
 */
void marine_ui_tick(uint32_t now_ms);

/**
 * @brief The mode whose screen is currently active (not the mode ring preview). Safe to read without the LVGL lock.
 */
marine_ui_mode_t marine_ui_get_active_mode(void);

/**
 * @brief Idle power settings. After dim_after_ms without input the backlight drops to dim_pct. After off_after_ms it goes to
 *        0 % unless the autopilot is engaged or an alarm is active (0 = never fully off). Defaults: 30 s, 10 %, 10 min.
 *        Call under the LVGL port lock, or before the first snapshot.
 */
void marine_ui_set_power(uint32_t dim_after_ms, uint8_t dim_pct, uint32_t off_after_ms);

#ifdef __cplusplus
}
#endif
