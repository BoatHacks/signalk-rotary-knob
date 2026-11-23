/* SPDX-License-Identifier: Apache-2.0
 * Pure logic of the knob with no ESP-IDF or FreeRTOS dependency, so it can be unit tested on a host (test/host). The
 * firmware components knob_model and knob_power call these. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Small JSON value helpers for SignalK stream values. They are not a JSON parser: the input is one value or one
 * flat object as text, and they must never read past the terminating NUL. --- */

/* A bare number at the start of the text (leading spaces allowed). false for null, strings, objects. */
bool knob_parse_num(const char *json, double *out);
/* "text" -> text, truncated to n-1 characters. false when the value is not a JSON string. */
bool knob_parse_str(const char *json, char *out, size_t n);
/* The number after "key": inside a flat object text. false when the key is missing or not a number. */
bool knob_obj_num(const char *json, const char *key, double *out);
bool knob_obj_str(const char *json, const char *key, char *out, size_t n);

/* --- Staleness --- */

/* A value is stale when its timeout is not 0 and it never arrived (updated_ms 0) or is older than the timeout. */
bool knob_stale_by(uint32_t now_ms, uint32_t updated_ms, uint32_t timeout_ms);

/* --- Steering indicator (LEDs) --- */

typedef enum { KNOB_STEER_NONE = 0, KNOB_STEER_PORT, KNOB_STEER_STBD, KNOB_STEER_ON_COURSE } knob_steer_t;

/* Target minus heading, wrapped to -180..180. Negative: the boat has to turn to port. */
float knob_course_error(float target_deg, float heading_deg);
/* PORT, STBD or ON_COURSE for an error. *on_course is the hysteresis state kept by the caller: on course is entered within
 * enter_deg and left when the error passes leave_deg. */
knob_steer_t knob_steer_step(float err_deg, bool *on_course, float enter_deg, float leave_deg);
/* Brightness 0..1000 of the night-time on-course colour: full for hold_ms after the change, then a linear fade over fade_ms. */
uint32_t knob_fade_permille(uint32_t elapsed_ms, uint32_t hold_ms, uint32_t fade_ms);

/* --- Alarm --- */

/* The anchor watch is armed but the position is stale, missing, or the fix is gone. Never within boot_grace_ms of boot. */
bool knob_gps_lost(bool anchor_armed, uint32_t now_ms, uint32_t boot_grace_ms, bool pos_valid, uint32_t pos_ms,
                   uint32_t lost_after_ms, bool gps_known, bool gps_fix);

/* --- Boot --- */

/* Whether the knob shows its mode screen instead of the boot splash. The splash with its phases stays until the SignalK token
 * is verified for the first time since boot (token_ok: the stream is being opened or is up). From then on the mode screen stays,
 * also when Wi-Fi or the stream drop later (the link overlay says so). *verified is the latch, kept by the caller, false at
 * boot. */
bool knob_boot_show_mode(bool token_ok, bool *verified);
/* The mode to open at boot from the stored last non-HID mode: stored when it is a valid mode (0..count-1) other than
 * hid_mode and settings_mode, else 0 (nothing stored, or a value from another firmware). */
int knob_boot_mode(int stored, int count, int hid_mode, int settings_mode);
/* Whether to switch to USB-HID at boot: HID was the last mode, a USB host is on the cable, the boot window is still open and
 * the user has not changed the mode since the restore. Otherwise the knob stays on the last non-HID mode. */
bool knob_boot_hid(bool hid_last, bool host_present, uint32_t now_ms, uint32_t window_ms, bool user_changed_mode);

/* --- Mode API and allow control (#3) --- */

/* Mode numbers follow marine_ui_mode_t: 0 autopilot, 1 lights, 2 anchor, 3 hid, 4 settings, 5 volume. The names are the
 * API's. The numbers are stored (last mode), so new modes are appended. */
#define KNOB_MODE_COUNT 6
#define KNOB_MODE_HID 3
#define KNOB_MODE_SETTINGS 4
#define KNOB_MODE_VOLUME 5
/* "autopilot", "lights", "anchor", "hid", "settings" or "volume" to 0..5, -1 for anything else (case sensitive). */
int knob_mode_from_name(const char *name);
/* 0..3 to its API name, "unknown" otherwise. */
const char *knob_mode_name(int mode);

typedef enum {
    KNOB_MODE_REQ_OK = 0,
    KNOB_MODE_REQ_BAD,      /* not a JSON object with a "mode" string, an unknown name, or settings: 400 */
    KNOB_MODE_REQ_NO_HOST,  /* USB-HID without a USB host on the cable: 409 */
} knob_mode_req_t;
/* Checks a PUT /api/v1/knob/mode body such as {"mode":"anchor"}. *mode is set when the result is OK. */
knob_mode_req_t knob_mode_request(const char *json, bool usb_host, int *mode);

/* --- Autopilot states (#7, signalk-autopilot raymarineST) --- */

typedef enum { KNOB_AP_STANDBY = 0, KNOB_AP_AUTO, KNOB_AP_WIND, KNOB_AP_ROUTE, KNOB_AP_UNKNOWN } knob_ap_state_t;
/* The provider's state string ("standby", "auto", "wind", "route") to the state, KNOB_AP_UNKNOWN for anything else or NULL. */
knob_ap_state_t knob_ap_state_from(const char *s);
/* The state's name for PUT .../state ("auto" ...), NULL for UNKNOWN. */
const char *knob_ap_state_name(knob_ap_state_t s);

/* Lines of the pilot dialog (long press while engaged): STANDBY first, the other engaged modes, the tacks in compass and wind
 * only, CANCEL last. Returns the count (at most 7). */
typedef enum { KNOB_PL_STANDBY = 0, KNOB_PL_COMPASS, KNOB_PL_WIND, KNOB_PL_ROUTE, KNOB_PL_TACK_PORT, KNOB_PL_TACK_STBD, KNOB_PL_CANCEL } knob_pilot_line_t;
int knob_pilot_lines(knob_ap_state_t current, knob_pilot_line_t *out, int max);

/* A target change split into the steps the pilot's keys know (+10, -10, +1, -1), tens first, the shorter way round:
 * from 350 to 5 is +10 +1 +1 +1 +1 +1. Returns the count, at most max (the rest is dropped). */
int knob_adjust_steps(int from_deg, int to_deg, int *out, int max);

/* Apparent wind angle in degrees, negative to port: the absolute angle 0..180 and the side, 'P' or 'S' ('S' for 0 and 180). */
int knob_wind_abs(float awa_deg, char *side);
/* Which way to steer for a cross-track error in metres (SignalK: positive = right of the track, steer left): -1 left, +1 right,
 * 0 within on_track_m. */
int knob_xte_steer(float xte_m, float on_track_m);

/* --- Volume mode (#6, signalk-jukebox) --- */

#define KNOB_VOL_STEP 2   /* percent per click */
/* The volume after a turn: KNOB_VOL_STEP percent per click, clamped to 0..100. */
int knob_vol_step(int pct, int clicks);
/* SignalK ratio 0..1 to percent 0..100, rounded and clamped. */
int knob_vol_pct(double ratio);
/* Splits entertainment.jukebox.zones.<id>.<key> into the zone id and the key (volume, muted, connected, ...). false for any
 * other path or ids that do not fit. */
bool knob_jukebox_zone_path(const char *path, char *id, size_t id_n, char *key, size_t key_n);

/* Whether autopilot and anchor commands may leave the knob: the stored allow control setting, unless the serial bench
 * override is set for this session (override -1 none, 0 off, 1 on). */
bool knob_allow_control(bool stored, int override);

/* --- Settings screen (#5, docs/ui-design.md section 3.5) --- */

/* The items, in the order of the list. */
typedef enum {
    KNOB_SET_BRIGHTNESS = 0, /* active brightness, % */
    KNOB_SET_DIM_AFTER,      /* s */
    KNOB_SET_DIM_LEVEL,      /* % */
    KNOB_SET_OFF_AFTER,      /* min, 0 = never */
    KNOB_SET_NIGHT,          /* 0 follow the server, 1 day, 2 night */
    KNOB_SET_LEDS,           /* 0 off (every LED, the alarm included), 1 on */
    KNOB_SET_HEADING,        /* 0 true, 1 magnetic */
    KNOB_SET_HID_PROFILE,    /* 0 chartplotter, 1 media, 2 navigation keys */
    KNOB_SET_ALLOW_CONTROL,  /* 0 off, 1 on (off to on needs the confirm turn) */
    KNOB_SET_DEMO,           /* 0 off, 1 on: the knob restarts into or out of demo mode */
    KNOB_SET_SETUP_AP,       /* action: open or close the setup access point */
    KNOB_SET_DEVICE_INFO,    /* read-only page */
    KNOB_SET_SIGNALK,        /* read-only page */
    KNOB_SET_RESET,          /* reset setup, 5 s hold */
    KNOB_SET_COUNT
} knob_setting_t;

typedef enum { KNOB_KIND_NUMBER = 0, KNOB_KIND_CHOICE, KNOB_KIND_ACTION, KNOB_KIND_PAGE } knob_setting_kind_t;

typedef struct {
    const char *name;           /* upper case, as shown */
    knob_setting_kind_t kind;
    const char *unit;           /* NUMBER: "%", "s", "min" */
    const int *values;          /* NUMBER: the steps a turn moves through, ascending */
    int n_values;
    const char *const *words;   /* CHOICE: the word for each value 0..n_values-1 */
} knob_setting_desc_t;

const knob_setting_desc_t *knob_setting_desc(knob_setting_t s);
/* The list position after a turn, wrapping at both ends. */
int knob_list_wrap(int index, int delta, int count);
/* The value after a turn: the next step in the item's list, clamped at the ends. A value between two steps moves to the
 * nearer one in the turn's direction first. Choices step through 0..n-1, clamped. Other kinds keep the value. */
int knob_setting_step(knob_setting_t s, int value, int delta);
/* Value as shown: "80 %", "30 s", "NEVER" (off after 0), "10 min", or the choice's word. */
void knob_setting_format(knob_setting_t s, int value, char *out, size_t n);

/* Confirm turn for allow control off to on (like engage): after the press, one click right within window_ms confirms, any
 * press cancels, a left turn is ignored, the time running out cancels. */
typedef enum { KNOB_CONFIRM_WAITING = 0, KNOB_CONFIRM_DONE, KNOB_CONFIRM_CANCELLED } knob_confirm_t;
typedef enum { KNOB_CONFIRM_EV_TURN = 0, KNOB_CONFIRM_EV_PRESS, KNOB_CONFIRM_EV_TICK } knob_confirm_ev_t;
knob_confirm_t knob_confirm_step(uint32_t started_ms, uint32_t now_ms, uint32_t window_ms, knob_confirm_ev_t ev, int turn);

#ifdef __cplusplus
}
#endif
