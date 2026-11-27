/* SPDX-License-Identifier: Apache-2.0
 * Gauge mode (#16): one SignalK value per page, as a number with its unit and an arc coloured by the path's meta.zones. The
 * rules as pure code: the meta (units, name, display scale, timeout, zones), the unit conversion, the range of the arc, the
 * zone a value is in, and what the screen shows. docs/plans/issue-16-gauge-mode.md has the decisions. No ESP-IDF, host tested. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNOB_GAUGE_MAX 4            /* pages */
#define KNOB_GAUGE_MAX_ZONES 6      /* zones kept per path, the rest are dropped */
#define KNOB_GAUGE_TIMEOUT_MS 30000 /* a value older than this shows ---, unless the meta has a timeout */

/* The SignalK zone states (alarm states of the specification), mildest first. NONE: no zone holds the value. */
typedef enum {
    KNOB_ZONE_NONE = 0,
    KNOB_ZONE_NOMINAL,
    KNOB_ZONE_NORMAL,
    KNOB_ZONE_ALERT,
    KNOB_ZONE_WARN,
    KNOB_ZONE_ALARM,
    KNOB_ZONE_EMERGENCY
} knob_zone_t;

/* "nominal", "normal", "alert", "warn", "alarm", "emergency" to the state, NONE for anything else. */
knob_zone_t knob_zone_from(const char *s);

/* What a state looks like on the knob: 0 normal (nominal, normal, none), 1 warning (alert, warn), 2 alarm (alarm, emergency). */
int knob_zone_level(knob_zone_t z);

typedef struct {
    double lower, upper;   /* SI units, -HUGE_VAL or HUGE_VAL for an open end */
    uint8_t state;         /* knob_zone_t */
    char message[24];
} knob_gauge_zone_t;

/* Takes meta.zones out of a meta object text: up to max zones, in their order. Returns the count, -1 when the text has no
 * "zones" array. A zone without a known state is dropped. */
int knob_gauge_parse_zones(const char *meta_json, knob_gauge_zone_t *out, int max);

/* The most severe zone that holds the SI value (lower <= v <= upper), -1 when none does. */
int knob_gauge_zone_at(const knob_gauge_zone_t *z, int n, double v);

/* What the knob knows of one gauge's path, from the stream: the value and the path's meta. SI units. */
typedef struct {
    bool has_value;
    double value;
    uint32_t value_ms;           /* knob time of arrival, 0 = never */
    bool seen;
    double seen_lo, seen_hi;     /* the lowest and highest value since boot */
    bool has_meta;
    char units[12];              /* meta.units, "" unknown */
    char meta_name[20];          /* meta.shortName, else "" */
    char display_name[24];       /* meta.displayName, else "" */
    bool has_scale;
    double scale_lo, scale_hi;   /* meta.displayScale */
    uint32_t timeout_ms;         /* meta.timeout, 0 = KNOB_GAUGE_TIMEOUT_MS */
    uint8_t n_zones;
    knob_gauge_zone_t zones[KNOB_GAUGE_MAX_ZONES];
} knob_gauge_src_t;

void knob_gauge_src_init(knob_gauge_src_t *g);
/* A meta item for the path: replaces units, names, scale, timeout and zones (a meta item carries the whole meta). */
void knob_gauge_meta(knob_gauge_src_t *g, const char *meta_json);
void knob_gauge_value(knob_gauge_src_t *g, double v, uint32_t now_ms);

/* The unit the knob shows for an SI unit: m/s as knots, K as degrees Celsius, a ratio as percent, rad as degrees, Pa as hPa,
 * Hz as rpm, C as Ah, J as Wh, m3 as litres. Any other unit is shown as it is. decimals -1: by the size of the value. */
typedef struct {
    char unit[8];
    double scale, offset;        /* display = si * scale + offset */
    int decimals;
} knob_gauge_unit_t;
void knob_gauge_unit(const char *si_units, knob_gauge_unit_t *out);

/* The arc's range in display units: meta.displayScale, else a default for the unit and the kind of path (depth 0..30 m, wind
 * 0..40 kn, boat speed 0..12 kn, a 12, 24 or 48 V battery ...), else around the zones' bounds, else the values seen, else
 * 0..100. The ones from the zones or the values grow to hold every value seen. */
void knob_gauge_range(const knob_gauge_src_t *g, const char *path, const knob_gauge_unit_t *u, double *lo, double *hi);

/* A short upper case name for a path: a table of the common ones (DEPTH, SOG, AWS, HOUSE BATTERY ...), else the last part of the
 * path split at its capitals (speedApparent: SPEED APPARENT). true when the name came from the table. */
bool knob_gauge_path_name(const char *path, char *out, size_t n);

/* The value as text with the decimals (-1: none for a whole number, else two below 10, one below 100, none from there). */
void knob_gauge_format(double v, int decimals, char *out, size_t n);

/* What the screen shows for one page, in display units and fractions of the arc (0 at its start, 1 at its end). */
typedef struct {
    char name[20];
    char unit[8];
    char text[12];               /* the value, "---" when there is none or it is stale */
    char lo_text[8], hi_text[8]; /* the ends of the range */
    float frac;                  /* where the value is on the arc, clamped */
    float origin;                /* where the fill starts: 0, or the zero of a range across it */
    uint8_t state;               /* knob_zone_t of the value, NONE without zones */
    char message[24];            /* the zone's message while it is not normal, else "" */
    bool valid;                  /* a value that is not stale */
    uint8_t n_zones;
    struct { float a, b; uint8_t state; } zones[KNOB_GAUGE_MAX_ZONES]; /* fractions, clipped to the range */
} knob_gauge_view_t;

/* name: the configured name, "" for meta.shortName, the path table, meta.displayName, then the path. */
void knob_gauge_view(const knob_gauge_src_t *g, const char *path, const char *name, uint32_t now_ms, knob_gauge_view_t *out);

/* Demo mode: simulated meta and values for page i (depth, speed, wind, battery, all with zones). */
void knob_gauge_demo(knob_gauge_src_t *g, int i, uint32_t now_ms);
/* The path the demo's page i stands for (the default paths), for knob_gauge_view's ranges and names. */
const char *knob_gauge_demo_path(int i);

#ifdef __cplusplus
}
#endif
