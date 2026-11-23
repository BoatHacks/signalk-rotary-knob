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
