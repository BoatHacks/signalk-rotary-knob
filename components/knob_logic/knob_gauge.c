/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_gauge.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "knob_logic.h"

/* ---- A small scanner for the meta object: top level keys and the elements of an array, strings and nesting respected. It
 * never reads past the NUL. ---- */

/* The end of the JSON value at p (one past it), or NULL when the text ends first. */
static const char *value_end(const char *p)
{
    int depth = 0;
    bool str = false;
    for (; *p; p++) {
        char c = *p;
        if (str) {
            if (c == '\\' && p[1]) p++;
            else if (c == '"') { str = false; if (depth == 0) return p + 1; }
            continue;
        }
        if (c == '"') str = true;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') { if (--depth <= 0) return depth < 0 ? p : p + 1; }
        else if (depth == 0 && (c == ',')) return p;
    }
    return depth == 0 && !str ? p : NULL;
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* The value of a key of the outer object: *v and *len, false when it is not there. */
static bool top_value(const char *j, const char *key, const char **v, size_t *len)
{
    if (!j) return false;
    const char *p = skip_ws(j);
    if (*p != '{') return false;
    p++;
    size_t kl = strlen(key);
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return false;
        if (*p != '"') return false;
        const char *k = p + 1, *ke = k;
        while (*ke && *ke != '"') { if (*ke == '\\' && ke[1]) ke++; ke++; }
        if (!*ke) return false;
        p = skip_ws(ke + 1);
        if (*p != ':') return false;
        p = skip_ws(p + 1);
        const char *e = value_end(p);
        if (!e) return false;
        if ((size_t)(ke - k) == kl && !strncmp(k, key, kl)) { *v = p; *len = (size_t)(e - p); return true; }
        p = skip_ws(e);
        if (*p == ',') p++;
    }
    return false;
}

/* A copy of a value span, cut to n-1 characters (a cut object or array is not used). */
static bool copy_span(const char *v, size_t len, char *out, size_t n)
{
    if (len + 1 > n) return false;
    memcpy(out, v, len);
    out[len] = '\0';
    return true;
}

/* Copies at most n-1 bytes and terminates (snprintf with %s warns about the cut at -Werror). */
static void cpy(char *d, size_t n, const char *s)
{
    if (!n) return;
    size_t l = s ? strlen(s) : 0;
    if (l >= n) l = n - 1;
    if (l) memcpy(d, s, l);
    d[l] = '\0';
}

static void upper_ascii(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

knob_zone_t knob_zone_from(const char *s)
{
    static const char *const w[] = { "nominal", "normal", "alert", "warn", "alarm", "emergency" };
    if (!s) return KNOB_ZONE_NONE;
    for (int i = 0; i < 6; i++) if (!strcmp(s, w[i])) return (knob_zone_t)(i + 1);
    return KNOB_ZONE_NONE;
}

int knob_zone_level(knob_zone_t z)
{
    if (z == KNOB_ZONE_ALERT || z == KNOB_ZONE_WARN) return 1;
    if (z == KNOB_ZONE_ALARM || z == KNOB_ZONE_EMERGENCY) return 2;
    return 0;
}

int knob_gauge_parse_zones(const char *meta, knob_gauge_zone_t *out, int max)
{
    const char *v;
    size_t len;
    if (!top_value(meta, "zones", &v, &len) || *v != '[') return -1;
    const char *p = v + 1, *end = v + len;
    int n = 0;
    while (p < end) {
        p = skip_ws(p);
        if (p >= end || *p == ']') break;
        const char *e = value_end(p);
        if (!e || e > end) break;
        char obj[200];
        if (*p == '{' && n < max && copy_span(p, (size_t)(e - p), obj, sizeof(obj))) {
            knob_gauge_zone_t z;
            char st[16] = "";
            memset(&z, 0, sizeof(z));
            const char *x;
            size_t xl;
            z.lower = -HUGE_VAL;
            z.upper = HUGE_VAL;
            if (top_value(obj, "lower", &x, &xl)) knob_parse_num(x, &z.lower);
            if (top_value(obj, "upper", &x, &xl)) knob_parse_num(x, &z.upper);
            if (top_value(obj, "state", &x, &xl)) knob_parse_str(x, st, sizeof(st));
            if (top_value(obj, "message", &x, &xl)) knob_parse_str(x, z.message, sizeof(z.message));
            z.state = (uint8_t)knob_zone_from(st);
            if (z.state != KNOB_ZONE_NONE) out[n++] = z;
        }
        p = skip_ws(e);
        if (p < end && *p == ',') p++;
    }
    return n;
}

int knob_gauge_zone_at(const knob_gauge_zone_t *z, int n, double v)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (v < z[i].lower || v > z[i].upper) continue;
        if (best < 0 || z[i].state > z[best].state) best = i;
    }
    return best;
}

void knob_gauge_src_init(knob_gauge_src_t *g)
{
    memset(g, 0, sizeof(*g));
}

void knob_gauge_meta(knob_gauge_src_t *g, const char *meta)
{
    const char *v;
    size_t len;
    g->has_meta = true;
    g->units[0] = g->meta_name[0] = g->display_name[0] = '\0';
    if (top_value(meta, "units", &v, &len)) knob_parse_str(v, g->units, sizeof(g->units));
    if (top_value(meta, "shortName", &v, &len)) knob_parse_str(v, g->meta_name, sizeof(g->meta_name));
    if (top_value(meta, "displayName", &v, &len)) knob_parse_str(v, g->display_name, sizeof(g->display_name));
    g->has_scale = false;
    char sc[160];
    if (top_value(meta, "displayScale", &v, &len) && *v == '{' && copy_span(v, len, sc, sizeof(sc))) {
        double lo, hi;
        const char *x;
        size_t xl;
        if (top_value(sc, "lower", &x, &xl) && knob_parse_num(x, &lo) && top_value(sc, "upper", &x, &xl) &&
            knob_parse_num(x, &hi) && hi > lo) {
            g->has_scale = true;
            g->scale_lo = lo;
            g->scale_hi = hi;
        }
    }
    double t;
    g->timeout_ms = 0;
    if (top_value(meta, "timeout", &v, &len) && knob_parse_num(v, &t) && t > 0 && t < 86400) g->timeout_ms = (uint32_t)(t * 1000.0);
    int n = knob_gauge_parse_zones(meta, g->zones, KNOB_GAUGE_MAX_ZONES);
    g->n_zones = (uint8_t)(n > 0 ? n : 0);
}

void knob_gauge_value(knob_gauge_src_t *g, double v, uint32_t now)
{
    if (!isfinite(v)) return;
    g->has_value = true;
    g->value = v;
    g->value_ms = now ? now : 1;
    if (!g->seen) { g->seen = true; g->seen_lo = g->seen_hi = v; }
    if (v < g->seen_lo) g->seen_lo = v;
    if (v > g->seen_hi) g->seen_hi = v;
}

void knob_gauge_unit(const char *si, knob_gauge_unit_t *u)
{
    static const struct { const char *si, *unit; double scale, offset; int dec; } t[] = {
        { "m/s", "kn", 1.9438445, 0, 1 },
        { "K", "\xC2\xB0" "C", 1, -273.15, 1 },
        { "ratio", "%", 100, 0, 0 },
        { "rad", "\xC2\xB0", 57.2957795, 0, 0 },
        { "rad/s", "\xC2\xB0/s", 57.2957795, 0, 1 },
        { "Pa", "hPa", 0.01, 0, 0 },
        { "Hz", "rpm", 60, 0, 0 },
        { "C", "Ah", 1.0 / 3600.0, 0, 0 },
        { "J", "Wh", 1.0 / 3600.0, 0, 0 },
        { "m3", "L", 1000, 0, 0 },
        { "m3/s", "L/h", 3600000, 0, 1 },
        { "V", "V", 1, 0, 2 },
        { "A", "A", 1, 0, 1 },
        { "W", "W", 1, 0, 0 },
        { "m", "m", 1, 0, 1 },
        { "s", "s", 1, 0, 0 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (si && !strcmp(si, t[i].si)) {
            snprintf(u->unit, sizeof(u->unit), "%s", t[i].unit);
            u->scale = t[i].scale;
            u->offset = t[i].offset;
            u->decimals = t[i].dec;
            return;
        }
    }
    snprintf(u->unit, sizeof(u->unit), "%s", si ? si : "");
    u->scale = 1;
    u->offset = 0;
    u->decimals = -1;
}

static double to_disp(const knob_gauge_unit_t *u, double si) { return si * u->scale + u->offset; }

static bool has(const char *path, const char *part) { return path && strstr(path, part) != NULL; }

/* The default range for the display unit and the kind of path, false when there is none. */
static bool unit_range(const knob_gauge_src_t *g, const char *path, const knob_gauge_unit_t *u, double *lo, double *hi)
{
    const char *x = u->unit;
    if (!strcmp(x, "m") && has(path, "depth")) { *lo = 0; *hi = 30; return true; }
    if (!strcmp(x, "kn")) { *lo = 0; *hi = has(path, "wind") ? 40 : 12; return true; }
    if (!strcmp(x, "\xC2\xB0" "C")) {
        bool hot = has(path, "propulsion") || has(path, "engine") || has(path, "coolant") || has(path, "exhaust");
        *lo = hot ? 0 : -10; *hi = hot ? 120 : 40; return true;
    }
    if (!strcmp(x, "%")) { *lo = 0; *hi = 100; return true; }
    if (!strcmp(x, "\xC2\xB0")) { *lo = 0; *hi = 360; return true; }
    if (!strcmp(x, "hPa")) { *lo = 960; *hi = 1050; return true; }
    if (!strcmp(x, "rpm")) { *lo = 0; *hi = 4000; return true; }
    if (!strcmp(x, "V") && g->seen) {
        double v = g->seen_hi;
        if (v <= 16) { *lo = 11; *hi = 15; return true; }
        if (v <= 32) { *lo = 22; *hi = 30; return true; }
        if (v <= 64) { *lo = 44; *hi = 58; return true; }
    }
    return false;
}

void knob_gauge_range(const knob_gauge_src_t *g, const char *path, const knob_gauge_unit_t *u, double *lo, double *hi)
{
    if (g->has_scale) {
        *lo = to_disp(u, g->scale_lo);
        *hi = to_disp(u, g->scale_hi);
        if (*hi < *lo) { double t = *lo; *lo = *hi; *hi = t; }
        return;
    }
    if (unit_range(g, path, u, lo, hi)) return;
    bool any = false;
    double a = 0, b = 0;
    for (int i = 0; i < g->n_zones; i++) {
        double e[2] = { g->zones[i].lower, g->zones[i].upper };
        for (int k = 0; k < 2; k++) {
            if (!isfinite(e[k])) continue;
            double d = to_disp(u, e[k]);
            if (!any) { a = b = d; any = true; }
            if (d < a) a = d;
            if (d > b) b = d;
        }
    }
    if (any) {
        double span = b - a;
        if (span <= 0) span = fabs(a) > 1 ? fabs(a) * 0.4 : 2;
        a -= span * 0.5;
        b += span * 0.5;
    }
    if (g->seen) {
        double sl = to_disp(u, g->seen_lo), sh = to_disp(u, g->seen_hi);
        if (sl > sh) { double t = sl; sl = sh; sh = t; }
        if (!any) {
            a = sl >= 0 ? 0 : sl;
            b = sh;
            if (b - a <= 0) { double p = fabs(b) > 1 ? fabs(b) * 0.25 : 1; a = sl >= 0 && sl - p < 0 ? 0 : sl - p; b = sh + p; }
            else b += (b - a) * 0.25;
            any = true;
        } else {
            if (sl < a) a = sl;
            if (sh > b) b = sh;
        }
    }
    if (!any) { a = 0; b = 100; }
    *lo = a;
    *hi = b;
}

bool knob_gauge_path_name(const char *path, char *out, size_t n)
{
    static const struct { const char *path, *name; } t[] = {
        { "environment.depth.belowTransducer", "DEPTH" },
        { "environment.depth.belowKeel", "DEPTH KEEL" },
        { "environment.depth.belowSurface", "DEPTH" },
        { "navigation.speedOverGround", "SOG" },
        { "navigation.speedThroughWater", "STW" },
        { "navigation.courseOverGroundTrue", "COG" },
        { "navigation.headingTrue", "HEADING" },
        { "navigation.headingMagnetic", "HEADING" },
        { "navigation.rateOfTurn", "RATE OF TURN" },
        { "environment.wind.speedApparent", "AWS" },
        { "environment.wind.speedTrue", "TWS" },
        { "environment.wind.angleApparent", "AWA" },
        { "environment.wind.angleTrueWater", "TWA" },
        { "environment.water.temperature", "WATER TEMP" },
        { "environment.outside.temperature", "AIR TEMP" },
        { "environment.outside.pressure", "PRESSURE" },
        { "environment.outside.humidity", "HUMIDITY" },
        { "environment.inside.temperature", "CABIN TEMP" },
    };
    if (!n) return false;
    out[0] = '\0';
    if (!path) return false;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (!strcmp(path, t[i].path)) { snprintf(out, n, "%s", t[i].name); return true; }
    }
    /* electrical.batteries.<id>.voltage: <ID> BATTERY; .stateOfCharge <ID> CHARGE; .current <ID> CURRENT */
    char id[24], key[32];
    if (sscanf(path, "electrical.batteries.%23[^.].%31s", id, key) == 2) {
        upper_ascii(id);
        const char *k = !strcmp(key, "voltage") ? "BATTERY" : !strcmp(key, "capacity.stateOfCharge") ? "CHARGE" :
                        !strcmp(key, "current") ? "CURRENT" : !strcmp(key, "power") ? "POWER" : NULL;
        if (k) { snprintf(out, n, "%s %s", id, k); return true; }
    }
    if (sscanf(path, "propulsion.%23[^.].%31s", id, key) == 2) {
        upper_ascii(id);
        const char *k = !strcmp(key, "revolutions") ? "RPM" : !strcmp(key, "temperature") ? "TEMP" :
                        !strcmp(key, "oilPressure") ? "OIL" : !strcmp(key, "coolantTemperature") ? "COOLANT" : NULL;
        if (k) { snprintf(out, n, "%s %s", id, k); return true; }
    }
    if (sscanf(path, "tanks.%23[^.].%31[^.]", id, key) == 2 && has(path, ".currentLevel")) {
        upper_ascii(id);
        upper_ascii(key);
        if (!strcmp(key, "0") || !strcmp(key, "MAIN")) snprintf(out, n, "%s", id);
        else snprintf(out, n, "%s %s", id, key);
        return true;
    }
    /* the last part, split at its capitals */
    const char *last = strrchr(path, '.');
    last = last ? last + 1 : path;
    size_t o = 0;
    for (const char *p = last; *p && o + 1 < n; p++) {
        if (*p >= 'A' && *p <= 'Z' && p != last && o + 2 < n) out[o++] = ' ';
        out[o++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    }
    out[o] = '\0';
    return false;
}

void knob_gauge_format(double v, int dec, char *out, size_t n)
{
    if (dec < 0) {
        double a = fabs(v);
        dec = fabs(v - round(v)) < 1e-9 ? 0 : a < 10 ? 2 : a < 100 ? 1 : 0;   /* whole numbers stay whole */
    }
    if (dec == 2 && fabs(v) >= 100) dec = 1;
    if (dec >= 1 && fabs(v) >= 1000) dec = 0;
    snprintf(out, n, "%.*f", dec, v);
    if (!strcmp(out, "-0") || !strcmp(out, "-0.0") || !strcmp(out, "-0.00")) memmove(out, out + 1, strlen(out));
}

static float frac_of(double v, double lo, double hi)
{
    if (!(hi > lo)) return 0;
    double f = (v - lo) / (hi - lo);
    return f < 0 ? 0.0f : f > 1 ? 1.0f : (float)f;
}

static void range_text(double v, double span, char *out, size_t n)
{
    knob_gauge_format(v, fabs(v - round(v)) < 1e-9 || span >= 10 ? 0 : 1, out, n);
}

void knob_gauge_view(const knob_gauge_src_t *g, const char *path, const char *name, uint32_t now, knob_gauge_view_t *o)
{
    memset(o, 0, sizeof(*o));
    if (name && name[0]) cpy(o->name, sizeof(o->name), name);
    else if (g->meta_name[0]) cpy(o->name, sizeof(o->name), g->meta_name);
    else if (!knob_gauge_path_name(path, o->name, sizeof(o->name)) && g->display_name[0])
        cpy(o->name, sizeof(o->name), g->display_name);   /* the server's name beats one made from the path */
    upper_ascii(o->name);

    knob_gauge_unit_t u;
    knob_gauge_unit(g->units, &u);
    cpy(o->unit, sizeof(o->unit), u.unit);
    double lo, hi;
    knob_gauge_range(g, path, &u, &lo, &hi);
    range_text(lo, hi - lo, o->lo_text, sizeof(o->lo_text));
    range_text(hi, hi - lo, o->hi_text, sizeof(o->hi_text));
    o->origin = lo < 0 && hi > 0 ? frac_of(0, lo, hi) : 0;

    o->n_zones = g->n_zones;
    for (int i = 0; i < g->n_zones; i++) {
        double a = isfinite(g->zones[i].lower) ? to_disp(&u, g->zones[i].lower) : lo;
        double b = isfinite(g->zones[i].upper) ? to_disp(&u, g->zones[i].upper) : hi;
        if (u.scale < 0) { double t = a; a = b; b = t; }
        o->zones[i].a = frac_of(a, lo, hi);
        o->zones[i].b = frac_of(b, lo, hi);
        o->zones[i].state = g->zones[i].state;
    }

    uint32_t to = g->timeout_ms ? g->timeout_ms : KNOB_GAUGE_TIMEOUT_MS;
    o->valid = g->has_value && !knob_stale_by(now, g->value_ms, to);
    if (!o->valid) {
        snprintf(o->text, sizeof(o->text), "---");
        o->frac = o->origin;
        return;
    }
    double d = to_disp(&u, g->value);
    knob_gauge_format(d, u.decimals, o->text, sizeof(o->text));
    o->frac = frac_of(d, lo, hi);
    int z = knob_gauge_zone_at(g->zones, g->n_zones, g->value);
    o->state = z >= 0 ? g->zones[z].state : KNOB_ZONE_NONE;
    if (z >= 0 && knob_zone_level((knob_zone_t)o->state) > 0) {
        cpy(o->message, sizeof(o->message), g->zones[z].message);
        upper_ascii(o->message);
    }
}

const char *knob_gauge_demo_path(int i)
{
    static const char *const p[KNOB_GAUGE_MAX] = { "environment.depth.belowTransducer", "navigation.speedOverGround",
                                                   "environment.wind.speedApparent", "electrical.batteries.house.voltage" };
    return i >= 0 && i < KNOB_GAUGE_MAX ? p[i] : "";
}

void knob_gauge_demo(knob_gauge_src_t *g, int i, uint32_t now)
{
    static const char *const meta[KNOB_GAUGE_MAX] = {
        "{\"units\":\"m\",\"shortName\":\"DEPTH\",\"zones\":[{\"upper\":2,\"state\":\"alarm\",\"message\":\"Too shallow\"},"
        "{\"lower\":2,\"upper\":4,\"state\":\"warn\",\"message\":\"Shallow water\"}]}",
        "{\"units\":\"m/s\",\"shortName\":\"SOG\"}",
        "{\"units\":\"m/s\",\"shortName\":\"AWS\",\"zones\":[{\"lower\":10.8,\"upper\":13.9,\"state\":\"warn\",\"message\":\"Reef now\"},"
        "{\"lower\":13.9,\"state\":\"alarm\",\"message\":\"Gale\"}]}",
        "{\"units\":\"V\",\"shortName\":\"HOUSE BATTERY\",\"zones\":[{\"upper\":11.8,\"state\":\"alarm\",\"message\":\"Battery flat\"},"
        "{\"lower\":11.8,\"upper\":12.2,\"state\":\"warn\",\"message\":\"Battery low\"}]}",
    };
    if (i < 0 || i >= KNOB_GAUGE_MAX) return;
    if (!g->has_meta) knob_gauge_meta(g, meta[i]);
    double t = now / 1000.0, v;
    switch (i) {
    case 0: v = 6.0 + 3.2 * sin(t / 11.0) + 0.3 * sin(t / 1.7); break;          /* 2.5 .. 9.5 m: into the shallow zones */
    case 1: v = (5.6 + 0.6 * sin(t / 9.0)) / 1.9438445; break;                   /* about 5.6 kn */
    case 2: v = (18.0 + 6.0 * sin(t / 13.0) + 1.5 * sin(t / 2.3)) / 1.9438445; break; /* 10 .. 25 kn: into reef and gale */
    default: v = 12.5 + 0.6 * sin(t / 17.0); break;                              /* 11.9 .. 13.1 V */
    }
    knob_gauge_value(g, v, now);
}
