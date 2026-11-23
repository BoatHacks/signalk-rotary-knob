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
 * hid_mode, else 0 (nothing stored, or a value from another firmware). */
int knob_boot_mode(int stored, int count, int hid_mode);
/* Whether to switch to USB-HID at boot: HID was the last mode, a USB host is on the cable, the boot window is still open and
 * the user has not changed the mode since the restore. Otherwise the knob stays on the last non-HID mode. */
bool knob_boot_hid(bool hid_last, bool host_present, uint32_t now_ms, uint32_t window_ms, bool user_changed_mode);

/* --- Mode API and allow control (#3) --- */

/* Mode numbers follow marine_ui_mode_t: 0 autopilot, 1 lights, 2 anchor, 3 hid. The names are the API's. */
#define KNOB_MODE_COUNT 4
#define KNOB_MODE_HID 3
/* "autopilot", "lights", "anchor" or "hid" to 0..3, -1 for anything else (case sensitive). */
int knob_mode_from_name(const char *name);
/* 0..3 to its API name, "unknown" otherwise. */
const char *knob_mode_name(int mode);

typedef enum {
    KNOB_MODE_REQ_OK = 0,
    KNOB_MODE_REQ_BAD,      /* not a JSON object with a "mode" string, or an unknown name: 400 */
    KNOB_MODE_REQ_NO_HOST,  /* USB-HID without a USB host on the cable: 409 */
} knob_mode_req_t;
/* Checks a PUT /api/v1/knob/mode body such as {"mode":"anchor"}. *mode is set when the result is OK. */
knob_mode_req_t knob_mode_request(const char *json, bool usb_host, int *mode);

/* Whether autopilot and anchor commands may leave the knob: the stored allow control setting, unless the serial bench
 * override is set for this session (override -1 none, 0 off, 1 on). */
bool knob_allow_control(bool stored, int override);

#ifdef __cplusplus
}
#endif
