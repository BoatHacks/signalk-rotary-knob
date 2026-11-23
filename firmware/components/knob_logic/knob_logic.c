/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_logic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool knob_parse_num(const char *j, double *out)
{
    if (!j) return false;
    while (*j == ' ') j++;
    if (!(*j == '-' || *j == '+' || *j == '.' || (*j >= '0' && *j <= '9'))) return false;
    char *end = NULL;
    double v = strtod(j, &end);
    if (end == j) return false;
    *out = v;
    return true;
}

bool knob_parse_str(const char *j, char *out, size_t n)
{
    if (!j || j[0] != '"' || n == 0) return false;
    size_t i = 0;
    for (const char *p = j + 1; *p && *p != '"' && i + 1 < n; p++) out[i++] = *p;
    out[i] = '\0';
    return true;
}

/* Position just after "key": (and spaces), or NULL. */
static const char *after_key(const char *j, const char *key)
{
    char pat[40];
    if (!j || !key || snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) return NULL;
    const char *p = strstr(j, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    return p;
}

bool knob_obj_num(const char *j, const char *key, double *out)
{
    const char *p = after_key(j, key);
    return p && knob_parse_num(p, out);
}

bool knob_obj_str(const char *j, const char *key, char *out, size_t n)
{
    const char *p = after_key(j, key);
    return p && knob_parse_str(p, out, n);
}

bool knob_stale_by(uint32_t now, uint32_t updated, uint32_t timeout)
{
    return timeout != 0 && (updated == 0 || now - updated > timeout);
}

float knob_course_error(float target, float heading)
{
    float e = fmodf(target - heading + 540.0f, 360.0f) - 180.0f;
    return e;
}

knob_steer_t knob_steer_step(float err, bool *on_course, float enter, float leave)
{
    float a = fabsf(err);
    if (*on_course ? a > leave : a <= enter) *on_course = !*on_course;
    if (*on_course) return KNOB_STEER_ON_COURSE;
    return err < 0 ? KNOB_STEER_PORT : KNOB_STEER_STBD;
}

uint32_t knob_fade_permille(uint32_t elapsed, uint32_t hold, uint32_t fade)
{
    if (elapsed < hold) return 1000;
    if (fade == 0) return 0;
    uint32_t f = elapsed - hold;
    return f >= fade ? 0 : 1000 - (1000 * f) / fade;
}

bool knob_gps_lost(bool armed, uint32_t now, uint32_t grace, bool pos_valid, uint32_t pos_ms, uint32_t lost_after,
                   bool gps_known, bool gps_fix)
{
    if (!armed || now <= grace) return false;
    return !pos_valid || now - pos_ms > lost_after || (gps_known && !gps_fix);
}

bool knob_boot_show_mode(bool token_ok, bool *verified)
{
    if (token_ok) *verified = true;
    return *verified;
}

int knob_boot_mode(int stored, int count, int hid_mode)
{
    if (stored < 0 || stored >= count || stored == hid_mode) return 0;
    return stored;
}

bool knob_boot_hid(bool hid_last, bool host_present, uint32_t now_ms, uint32_t window_ms, bool user_changed_mode)
{
    return hid_last && host_present && now_ms < window_ms && !user_changed_mode;
}

static const char *const s_mode_names[KNOB_MODE_COUNT] = { "autopilot", "lights", "anchor", "hid", "settings" };

int knob_mode_from_name(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < KNOB_MODE_COUNT; i++) {
        if (!strcmp(name, s_mode_names[i])) return i;
    }
    return -1;
}

const char *knob_mode_name(int mode)
{
    return (mode >= 0 && mode < KNOB_MODE_COUNT) ? s_mode_names[mode] : "unknown";
}

knob_mode_req_t knob_mode_request(const char *json, bool usb_host, int *mode)
{
    char name[16];
    if (!json || !knob_obj_str(json, "mode", name, sizeof(name))) return KNOB_MODE_REQ_BAD;
    int m = knob_mode_from_name(name);
    if (m < 0 || m == KNOB_MODE_SETTINGS) return KNOB_MODE_REQ_BAD; /* the settings screen is opened on the knob only */
    if (m == KNOB_MODE_HID && !usb_host) return KNOB_MODE_REQ_NO_HOST;
    *mode = m;
    return KNOB_MODE_REQ_OK;
}

bool knob_allow_control(bool stored, int override)
{
    return override < 0 ? stored : override != 0;
}

static const int s_brightness[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const int s_dim_after[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 300, 600, 1800, 3600 };
static const int s_dim_level[] = { 1, 2, 5, 10, 15, 20, 30, 40, 50, 60, 80, 100 };
static const int s_off_after[] = { 0, 1, 2, 5, 10, 15, 30, 60, 120, 240 };
static const char *const s_night_w[] = { "SERVER", "DAY", "NIGHT" };
static const char *const s_onoff_w[] = { "OFF", "ON" };
static const char *const s_heading_w[] = { "TRUE", "MAGNETIC" };
static const char *const s_hid_w[] = { "CHARTPLOTTER", "MEDIA", "NAV KEYS" };

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static const knob_setting_desc_t s_settings[KNOB_SET_COUNT] = {
    [KNOB_SET_BRIGHTNESS] = { "BRIGHTNESS", KNOB_KIND_NUMBER, "%", s_brightness, N(s_brightness), NULL },
    [KNOB_SET_DIM_AFTER] = { "DIM AFTER", KNOB_KIND_NUMBER, "s", s_dim_after, N(s_dim_after), NULL },
    [KNOB_SET_DIM_LEVEL] = { "DIM LEVEL", KNOB_KIND_NUMBER, "%", s_dim_level, N(s_dim_level), NULL },
    [KNOB_SET_OFF_AFTER] = { "OFF AFTER", KNOB_KIND_NUMBER, "min", s_off_after, N(s_off_after), NULL },
    [KNOB_SET_NIGHT] = { "NIGHT MODE", KNOB_KIND_CHOICE, NULL, NULL, 3, s_night_w },
    [KNOB_SET_LEDS] = { "LEDS", KNOB_KIND_CHOICE, NULL, NULL, 2, s_onoff_w },
    [KNOB_SET_HEADING] = { "HEADING", KNOB_KIND_CHOICE, NULL, NULL, 2, s_heading_w },
    [KNOB_SET_HID_PROFILE] = { "HID PROFILE", KNOB_KIND_CHOICE, NULL, NULL, 3, s_hid_w },
    [KNOB_SET_ALLOW_CONTROL] = { "ALLOW CONTROL", KNOB_KIND_CHOICE, NULL, NULL, 2, s_onoff_w },
    [KNOB_SET_SETUP_AP] = { "SETUP ACCESS POINT", KNOB_KIND_ACTION, NULL, NULL, 0, NULL },
    [KNOB_SET_DEVICE_INFO] = { "DEVICE INFO", KNOB_KIND_PAGE, NULL, NULL, 0, NULL },
    [KNOB_SET_SIGNALK] = { "SIGNALK", KNOB_KIND_PAGE, NULL, NULL, 0, NULL },
    [KNOB_SET_RESET] = { "RESET SETUP", KNOB_KIND_ACTION, NULL, NULL, 0, NULL },
};

const knob_setting_desc_t *knob_setting_desc(knob_setting_t s)
{
    return (s >= 0 && s < KNOB_SET_COUNT) ? &s_settings[s] : NULL;
}

int knob_list_wrap(int index, int delta, int count)
{
    if (count <= 0) return 0;
    int i = (index + delta) % count;
    return i < 0 ? i + count : i;
}

int knob_setting_step(knob_setting_t s, int value, int delta)
{
    const knob_setting_desc_t *d = knob_setting_desc(s);
    if (!d || delta == 0) return value;
    if (d->kind == KNOB_KIND_CHOICE) {
        int v = value + delta;
        if (v < 0) v = 0;
        if (v > d->n_values - 1) v = d->n_values - 1;
        return v;
    }
    if (d->kind != KNOB_KIND_NUMBER) return value;
    /* Position in the list: the exact step, or between two steps. */
    int lo = -1, hi = d->n_values; /* largest step <= value, smallest step >= value */
    for (int i = 0; i < d->n_values; i++) {
        if (d->values[i] <= value) lo = i;
        if (d->values[i] >= value && hi == d->n_values) hi = i;
    }
    int i;
    if (delta > 0) i = (lo >= 0 && d->values[lo] == value) ? lo + delta : hi + delta - 1;
    else i = (hi < d->n_values && d->values[hi] == value) ? hi + delta : lo + delta + 1;
    if (i < 0) i = 0;
    if (i > d->n_values - 1) i = d->n_values - 1;
    return d->values[i];
}

void knob_setting_format(knob_setting_t s, int value, char *out, size_t n)
{
    const knob_setting_desc_t *d = knob_setting_desc(s);
    if (!out || n == 0) return;
    out[0] = '\0';
    if (!d) return;
    if (d->kind == KNOB_KIND_CHOICE) {
        snprintf(out, n, "%s", (value >= 0 && value < d->n_values) ? d->words[value] : "?");
    } else if (d->kind == KNOB_KIND_NUMBER) {
        if (s == KNOB_SET_OFF_AFTER && value == 0) snprintf(out, n, "NEVER");
        else snprintf(out, n, "%d %s", value, d->unit);
    }
}

knob_confirm_t knob_confirm_step(uint32_t started_ms, uint32_t now_ms, uint32_t window_ms, knob_confirm_ev_t ev, int turn)
{
    if (now_ms - started_ms >= window_ms) return KNOB_CONFIRM_CANCELLED;
    if (ev == KNOB_CONFIRM_EV_PRESS) return KNOB_CONFIRM_CANCELLED;
    if (ev == KNOB_CONFIRM_EV_TURN && turn > 0) return KNOB_CONFIRM_DONE;
    return KNOB_CONFIRM_WAITING;
}
