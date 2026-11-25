#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The modes, in mode ring order. The ring spaces the visible ones evenly, clockwise from the top: USB only while a USB
 *        host is on the cable, SETTINGS last and hidden while an alarm is active (docs/ui-design.md section 2).
 */
typedef enum {
    MARINE_MODE_AUTOPILOT = 0,
    MARINE_MODE_DIMMER,
    MARINE_MODE_ANCHOR_WATCH,
    MARINE_MODE_USB_HID,
    MARINE_MODE_SETTINGS,
    MARINE_MODE_MUSIC,          // appended: the numbers are stored as the last mode. On the ring it sits after ANCHOR.
    MARINE_MODE_MOTOR,          // appended (#8). On the ring it sits after AUTO.
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
    MARINE_ACT_ANCHOR_DROP,       // value = 1 (watch at the current position with the chosen radius)
    MARINE_ACT_ANCHOR_MOVE_DROP,    // value = 1
    MARINE_ACT_ANCHOR_RAISE,      // value = 1 (stop the watch, the anchor is raised)
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
    MARINE_ACT_ALARM_LED_SLOW_PULSE,// value = 1 slow red pulse (alarm acknowledged, still active), 0 = stop
    MARINE_ACT_MODE_CHANGED,        // value = marine_ui_mode_t the user switched to on the mode ring (to restore it at boot)
    MARINE_ACT_SETTING_SET,         // value = knob_setting_t, meta = the new value (settings screen, stored by the firmware)
    MARINE_ACT_SETUP_AP,            // value = 1 open the setup access point next to the Wi-Fi, 0 close it
    MARINE_ACT_VOL_SET,             // value = volume 0..100 %, meta = target (0 master, 1.. zone)
    MARINE_ACT_VOL_CONTROL,         // value = marine_vol_control_t
    MARINE_ACT_AP_SET_STATE,        // value = knob_ap_state_t (1 compass, 2 wind, 3 route) from the pilot dialog
    MARINE_ACT_AP_TACK,             // value = -1 port, +1 starboard, after the confirm turn
    MARINE_ACT_MOTOR_TURN,          // value = +1 or -1, one encoder click on the motor screen (sent at once, not batched)
    MARINE_ACT_MOTOR_LONG,          // value = 1, long press on the motor screen: arm, neutral, or disarm (knob_motor decides)
    MARINE_ACT_SETTING_PREVIEW,     // value = knob_setting_t, meta = a value to show at once without storing (brightness)
    MARINE_ACT_MOTOR_ACK            // value = 1, double press while the limit warning shows: lifts both limits
} marine_ui_action_type_t;

typedef struct {
    marine_ui_mode_t mode;
    marine_ui_action_type_t action;
    int32_t value;
    int32_t meta;
} marine_ui_action_t;

typedef void (*marine_ui_action_cb_t)(const marine_ui_action_t *act, void *user_ctx);

/**
 * @brief What became of the last anchor action, reported by the firmware in the snapshot (action_result and
 *        action_result_seq). The UI shows a pending screen after it sends an anchor action and ends it with the result.
 */
typedef enum {
    MARINE_RESULT_NONE = 0,
    MARINE_RESULT_OK,           // the server accepted the call (the new state follows through the data stream)
    MARINE_RESULT_LIVE_OFF,     // allow control is off, nothing was sent
    MARINE_RESULT_NO_LINK,      // not online, nothing was sent
    MARINE_RESULT_NO_POSITION,  // no fresh position, nothing was sent
    MARINE_RESULT_FAILED        // the server refused the call or did not answer
} marine_action_result_t;

typedef enum { MARINE_VOL_PLAY = 0, MARINE_VOL_PAUSE, MARINE_VOL_NEXT, MARINE_VOL_PREVIOUS } marine_vol_control_t;
typedef enum { MARINE_PLAY_STOPPED = 0, MARINE_PLAY_PLAYING, MARINE_PLAY_PAUSED } marine_play_state_t;
#define MARINE_MAX_VOL_ZONES 4

typedef struct {
    char name[16];           // MASTER, or the zone's name (its id until the name is known)
    uint8_t pct;             // 0..100
    bool connected;          // a zone's client is there (master: always)
    bool muted;              // a zone is muted (master: never, the jukebox has no master mute)
    uint32_t updated_ms;     // 0 = never received
    uint32_t timeout_ms;     // 0 = event driven (master)
} marine_vol_target_t;

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
 * Stream down (`signalk_connected == false`) shows LINK LOST, or CONNECTING with `boot_phase` while no data has arrived since
 * boot (`last_data_ms == 0`).
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
    bool anchor_dropped;
    uint16_t anchor_radius_m;
    float anchor_dist_m;
    float anchor_bearing_deg;
    uint32_t anchor_updated_ms;
    uint32_t anchor_timeout_ms;
    marine_alarm_type_t active_alarm;
    bool alarm_acknowledged;
    uint8_t action_result;        // marine_action_result_t of the latest anchor action
    uint16_t action_result_seq;   // changes with every new result, 0 = none yet

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

    // Allow control went on from the web page or the API: the UI shows the CONTROL ON notice for 3 s whenever this changes
    // to a new non-zero value. 0 = never.
    uint16_t control_on_seq;

    // Settings screen (#5): the stored value of each knob_setting_t item, and the read-only pages. Filled while the settings
    // screen is open.
    int16_t setting[24];
    bool setup_ap_open;
    uint16_t setup_ap_left_s;      // until the access point closes by itself
    char info_firmware[24];
    char info_wifi[33];            // network name, empty while not joined
    int8_t info_rssi;
    char info_ip[16];
    uint32_t info_free_kb;         // free internal RAM
    uint32_t info_uptime_s;
    char info_server[40];          // SignalK server host and port
    char info_link[12];            // online, connecting, ...
    char info_token[12];           // approved, waiting, none
    bool info_allow_control;       // what applies now (stored setting or the serial override)

    bool updating;                 // a firmware update is being installed: the UPDATING FIRMWARE notice is shown
    bool demo;                     // demo mode (#4): simulated data, the DEMO tag is shown on every screen

    // Autopilot states (#7): what the pilot does while engaged, and the data the wind and route screens show.
    uint8_t ap_state;              // knob_ap_state_t: 0 standby, 1 compass (auto), 2 wind, 3 route, 4 unknown
    float wind_awa_deg;            // apparent wind angle, negative to port
    uint32_t wind_updated_ms;
    uint32_t wind_timeout_ms;
    bool wind_target_valid;
    float wind_target_deg;         // the pilot's wind angle target, negative to port
    bool wind_adjust_supported;    // the provider takes target changes in wind (upstream fix), else ADJUST ON PILOT
    char route_wpt[16];            // next waypoint, "" none
    float route_brg_deg;           // bearing to it
    float route_dist_nm;
    float route_xte_m;             // cross-track error, SignalK sign: positive = right of the track, steer left
    uint32_t route_updated_ms;
    uint32_t route_timeout_ms;

    // Music (#6, signalk-jukebox): target 0 is MASTER, then the zones.
    bool music_mode;              // the MUSIC slot is on the ring (knob.music_mode)
    bool ap_mode, lights_mode, anchor_mode, usb_mode; // those slots are on the ring (MODES page, knob.<mode>_mode)
    uint8_t vol_target_count;
    marine_vol_target_t vol_targets[1 + MARINE_MAX_VOL_ZONES];
    uint8_t play_state;            // marine_play_state_t

    // Motor (#8): the knob's throttle state (knob_motor in knob_logic) and what the motor reports.
    bool motor_mode;               // the MOTOR slot is on the ring (knob.motor_mode)
    uint8_t motor_state;           // knob_motor_state_t: 0 disarmed, 1 confirm prompt, 2 armed
    int8_t motor_set_pct;          // the knob's throttle, -100 (astern) .. 100 (ahead)
    bool motor_limit_warning;      // a limit was hit, a double press lifts both
    bool motor_limit_lifted;
    uint8_t motor_max_ahead_pct;
    uint8_t motor_max_astern_pct;
    uint8_t motor_why;             // knob_motor_why_t: the last refusal or disarm reason
    uint32_t motor_why_ms;         // when it happened, 0 never
    uint32_t motor_confirm_ms;     // when the confirm prompt opened
    uint8_t motor_gear;            // knob_gear_t: 0 unknown, 1 forward, 2 neutral, 3 reverse, 4 fault
    uint32_t motor_gear_ms;        // 0 never
    float motor_rpm;               // revolutions per minute
    uint32_t motor_rpm_ms;
    float motor_torque_pct;        // engineTorque as a percentage
    uint32_t motor_torque_ms;
    float motor_power_w;           // from knob.motor_power_path
    uint32_t motor_power_ms;
    float motor_reported_pct;      // the throttle the motor reports, -100..100
    uint32_t motor_reported_ms;
    char track[40];
    char artist[32];
    int32_t track_pos_s;           // playback position in seconds, -1 unknown (shown as 0:53 under the volume while known)
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
 * @brief The light group selected on the lights screen and the target selected on the music screen (0 master, then the
 *        zones), for the LED levels (#9). Safe to read without the LVGL lock (single bytes).
 */
uint8_t marine_ui_get_selected_light(void);
uint8_t marine_ui_get_selected_vol_target(void);

/**
 * @brief Opens a mode without the mode ring and without emitting MARINE_ACT_MODE_CHANGED, for the boot restore. A system
 *        screen (splash, setup, pairing) stays on top until the snapshot leaves it. Call under the LVGL port lock.
 */
void marine_ui_set_mode(marine_ui_mode_t mode);

/**
 * @brief Idle power settings. After dim_after_ms without input the backlight drops to dim_pct (0 = never dims). After
 *        off_after_ms it goes to
 *        0 % unless the autopilot is engaged or an alarm is active (0 = never fully off). Defaults: 30 s, 10 %, 10 min.
 *        Call under the LVGL port lock, or before the first snapshot.
 */
void marine_ui_set_power(uint32_t dim_after_ms, uint8_t dim_pct, uint32_t off_after_ms);

/* ---- Automatic demo (#10): the overlay that shows the simulated inputs, and what the script asks the UI ----
 * All under the LVGL port lock. The overlay is only drawn while the automatic demo runs. */
typedef enum {
    MARINE_DEMO_FX_TURN = 0,  // a: direction (+1 clockwise, -1), b: clicks so far in this turn
    MARINE_DEMO_FX_PRESS,
    MARINE_DEMO_FX_DOUBLE,
    MARINE_DEMO_FX_LONG,      // a hold starts: a ring fills for the long press time
    MARINE_DEMO_FX_TOUCH,     // a: x, b: y
    MARINE_DEMO_FX_COUNT,     // a: countdown digit, 0 hides it
    MARINE_DEMO_FX_CLEAR      // hide everything
} marine_demo_fx_t;
void marine_ui_demo_fx(marine_demo_fx_t fx, int a, int b);
void marine_ui_demo_caption(const char *text);   // NULL hides it
bool marine_ui_ring_is_open(void);
marine_ui_mode_t marine_ui_ring_focus(void);     // the mode the ring's focus is on (the active mode while it is closed)
int marine_ui_settings_index(void);              // the focused settings item, -1 outside the settings list
#ifdef __cplusplus
}
#endif
