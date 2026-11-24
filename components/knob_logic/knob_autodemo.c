/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_autodemo.h"

#include <stddef.h>

#include "knob_logic.h"

/* Mode numbers (marine_ui_mode_t, knob_logic.h): 0 autopilot, 1 lights, 2 anchor, 3 USB-HID, 4 settings, 5 music. */
#define M_AUTOPILOT 0
#define M_LIGHTS    1
#define M_ANCHOR    2
#define M_USB       KNOB_MODE_HID
#define M_SETTINGS  KNOB_MODE_SETTINGS
#define M_MUSIC     KNOB_MODE_MUSIC

#define S(op_, a_, b_, ms_, t_) { (uint8_t)(op_), (int16_t)(a_), (int16_t)(b_), (uint16_t)(ms_), (t_) }
#define TURN(n)       S(KNOB_AD_TURN, n, 0, 0, NULL)
#define TURN_FAST(n)  S(KNOB_AD_TURN, n, 0, 140, NULL)
#define PRESS         S(KNOB_AD_PRESS, 0, 0, 0, NULL)
#define LONG          S(KNOB_AD_LONG, 0, 0, 0, NULL)
#define WAIT(ms)      S(KNOB_AD_WAIT, 0, 0, ms, NULL)
#define UNTIL(c, ms)  S(KNOB_AD_UNTIL, c, 0, ms, NULL)
#define RING_TO(m)    S(KNOB_AD_RING_TO, m, 0, 0, NULL)
#define SET_TO(i)     S(KNOB_AD_SET_TO, i, 0, 0, NULL)
#define EVENT(e)      S(KNOB_AD_EVENT, e, 0, 0, NULL)
#define CAPTION(t)    S(KNOB_AD_CAPTION, 0, 0, 0, t)
#define COUNTDOWN(n)  S(KNOB_AD_COUNTDOWN, n, 0, 0, NULL)
#define END           S(KNOB_AD_END, 0, 0, 0, NULL)

/* The owner's script (2026-11-24). About two and a half minutes. Every mode change goes through the ring. */
const knob_ad_step_t knob_ad_script[] = {
    EVENT(KNOB_AD_EV_RESET), CAPTION(NULL),
    COUNTDOWN(3),
    EVENT(KNOB_AD_EV_BOOT), UNTIL(KNOB_AD_COND_BOOT_DONE, 12000), WAIT(1500),

    /* Autopilot: engage, 23 degrees to port (one press for 10 degree steps, two clicks, back to 1 degree, three clicks) */
    CAPTION("ENGAGE"), LONG, WAIT(700), TURN(1), UNTIL(KNOB_AD_COND_ENGAGED, 4000), WAIT(1500),
    CAPTION("23\xC2\xB0 TO PORT"), PRESS, WAIT(600), TURN(-2), WAIT(700), PRESS, WAIT(600), TURN(-3), WAIT(600),
    CAPTION("HOLDING COURSE"), UNTIL(KNOB_AD_COND_HEADING_SETTLED, 20000), WAIT(2000),

    /* Lights: 80 to 20 % in 5 % clicks, then off */
    CAPTION("LIGHTS"), RING_TO(M_LIGHTS), WAIT(1200),
    CAPTION("DIM TO 20 %"), TURN(-12), WAIT(1500),
    CAPTION("LIGHT OFF"), LONG, WAIT(2000),

    /* Anchor: radius 35 to 20 m in 5 m clicks, apply, drop */
    CAPTION("ANCHOR"), RING_TO(M_ANCHOR), WAIT(1200),
    CAPTION("RADIUS 20 M"), TURN(-3), WAIT(800), PRESS, WAIT(1500),
    CAPTION("DROP ANCHOR"), LONG, WAIT(1000), PRESS, UNTIL(KNOB_AD_COND_ANCHOR_DROPPED, 9000), WAIT(2500),

    /* Music: volume 42 to 100 % in 2 % clicks, then the drag alarm */
    CAPTION("MUSIC"), RING_TO(M_MUSIC), WAIT(2000),
    CAPTION("VOLUME UP"), TURN_FAST(29), WAIT(2000),
    CAPTION(NULL), EVENT(KNOB_AD_EV_DRAG), UNTIL(KNOB_AD_COND_ALARM, 20000), WAIT(2500),
    CAPTION("ACKNOWLEDGE"), PRESS, WAIT(2000),

    /* Back to the anchor: radius 30 m clears the alarm */
    CAPTION("ANCHOR"), RING_TO(M_ANCHOR), WAIT(1200),
    CAPTION("RADIUS 30 M"), TURN(2), WAIT(800), PRESS, UNTIL(KNOB_AD_COND_NO_ALARM, 12000), WAIT(2500),

    /* USB: cursor keys to media (the profile order is chartplotter, media, cursor keys) */
    CAPTION("USB"), RING_TO(M_USB), WAIT(1500),
    CAPTION("MEDIA KEYS"), LONG, WAIT(1300), LONG, WAIT(2200),

    /* Setup: demo mode off. In the automatic demo the store is shown, not applied, and the script starts again. */
    CAPTION("SETUP"), RING_TO(M_SETTINGS), WAIT(1200),
    CAPTION("DEMO MODE OFF"), SET_TO(KNOB_SET_DEMO), WAIT(900), PRESS, WAIT(900), TURN(-3), WAIT(900), PRESS, WAIT(3000),
    /* three clicks: OFF from AUTO or QUIET (a choice stops at its first value) */
    CAPTION(NULL), WAIT(2000),
    END,
};

/* Steps through a press, a long press, a double press, a touch and a turn are small state machines on p->sub, timed
 * with p->next_ms. A step is done when do_step() returns true. */

static void btn(knob_ad_t *p, const knob_ad_ops_t *o, bool down)
{
    p->btn_down = down;
    if (o->button) o->button(o->ctx, down);
}

static void fx(const knob_ad_ops_t *o, knob_ad_fx_t f, int a, int b)
{
    if (o->show) o->show(o->ctx, f, a, b);
}

static int query(const knob_ad_ops_t *o, knob_ad_query_t q) { return o->query ? o->query(o->ctx, q) : -1; }

static bool due(const knob_ad_t *p, uint32_t now) { return (int32_t)(now - p->next_ms) >= 0; }

/* A short press: down, up after KNOB_AD_PRESS_MS, then KNOB_AD_SETTLE_MS for the UI to resolve it. */
static bool do_press(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o)
{
    switch (p->sub) {
    case 0: fx(o, KNOB_AD_FX_PRESS, 0, 0); btn(p, o, true); p->next_ms = now + KNOB_AD_PRESS_MS; p->sub = 1; return false;
    case 1: if (!due(p, now)) return false; btn(p, o, false); p->next_ms = now + KNOB_AD_SETTLE_MS; p->sub = 2; return false;
    default: return due(p, now);
    }
}

static bool do_long(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o)
{
    switch (p->sub) {
    case 0: fx(o, KNOB_AD_FX_LONG, 0, 0); btn(p, o, true); p->next_ms = now + KNOB_AD_LONG_MS; p->sub = 1; return false;
    case 1: if (!due(p, now)) return false; btn(p, o, false); p->next_ms = now + 150; p->sub = 2; return false;
    default: return due(p, now);
    }
}

static bool do_double(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o)
{
    switch (p->sub) {
    case 0: fx(o, KNOB_AD_FX_DOUBLE, 0, 0); btn(p, o, true); p->next_ms = now + KNOB_AD_PRESS_MS; p->sub = 1; return false;
    case 1: if (!due(p, now)) return false; btn(p, o, false); p->next_ms = now + KNOB_AD_DOUBLE_GAP_MS; p->sub = 2; return false;
    case 2: if (!due(p, now)) return false; btn(p, o, true); p->next_ms = now + KNOB_AD_PRESS_MS; p->sub = 3; return false;
    case 3: if (!due(p, now)) return false; btn(p, o, false); p->next_ms = now + KNOB_AD_SETTLE_MS; p->sub = 4; return false;
    default: return due(p, now);
    }
}

/* One click now, the next after the gap. The count goes to the overlay with every click. */
static bool do_turn(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, int clicks, uint16_t gap)
{
    int n = clicks < 0 ? -clicks : clicks;
    int dir = clicks < 0 ? -1 : 1;
    if (p->count >= n) return due(p, now);
    if (p->count > 0 && !due(p, now)) return false;
    if (o->turn) o->turn(o->ctx, dir);
    p->count++;
    fx(o, KNOB_AD_FX_TURN, dir, p->count);
    p->next_ms = now + (gap ? gap : KNOB_AD_CLICK_GAP_MS);
    return false;
}

static bool do_touch(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, int16_t x, int16_t y)
{
    switch (p->sub) {
    case 0:
        fx(o, KNOB_AD_FX_TOUCH, x, y);
        p->touch_down = true; p->touch_x = x; p->touch_y = y;
        if (o->touch) o->touch(o->ctx, x, y, true);
        p->next_ms = now + 120; p->sub = 1; return false;
    case 1:
        if (!due(p, now)) return false;
        p->touch_down = false;
        if (o->touch) o->touch(o->ctx, x, y, false);
        p->next_ms = now + KNOB_AD_SETTLE_MS; p->sub = 2; return false;
    default: return due(p, now);
    }
}

/* Ring: open it with a double press, turn one click at a time until the focus is on the mode, press. At most a full turn
 * of the ring; a mode that is not on the ring is skipped after that. */
static bool do_ring_to(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, int mode)
{
    switch (p->sub) {
    case 0:
        if (query(o, KNOB_AD_Q_MODE) == mode) return true;
        p->sub = 1;
        /* fall through */
    case 1: case 2: case 3: case 4: case 5: {
        uint8_t s = p->sub;
        p->sub = (uint8_t)(s - 1);              /* do_double counts from 0 */
        bool done = do_double(p, now, o);
        p->sub = (uint8_t)(p->sub + 1);
        if (!done) return false;
        p->sub = 10; p->count = 0; p->next_ms = now + 500; return false;
    }
    case 10:
        if (!due(p, now)) return false;
        if (query(o, KNOB_AD_Q_RING_FOCUS) == mode || p->count >= 8) { p->sub = 11; p->next_ms = now + 600; return false; }
        if (o->turn) o->turn(o->ctx, 1);
        p->count++;
        fx(o, KNOB_AD_FX_TURN, 1, p->count);
        p->next_ms = now + 450;
        return false;
    case 11: case 12: case 13: {
        if (!due(p, now)) return false;
        p->sub = (uint8_t)(p->sub - 11);
        bool done = do_press(p, now, o);
        p->sub = (uint8_t)(p->sub + 11);
        if (!done) return false;
        p->sub = 20; p->next_ms = now + 400; return false;
    }
    default:
        return due(p, now);
    }
}

static bool do_set_to(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, int item)
{
    if (p->count > 0 && !due(p, now)) return false;
    int at = query(o, KNOB_AD_Q_SET_INDEX);
    if (at == item || at < 0 || p->count >= 40) return true;
    int dir = item > at ? 1 : -1;
    if (o->turn) o->turn(o->ctx, dir);
    p->count++;
    fx(o, KNOB_AD_FX_TURN, dir, p->count);
    p->next_ms = now + 330;
    return false;
}

static bool do_countdown(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, int from)
{
    if (p->count > 0 && !due(p, now)) return false;
    if (p->count >= from) { fx(o, KNOB_AD_FX_COUNT, 0, 0); return true; }
    fx(o, KNOB_AD_FX_COUNT, from - p->count, 0);
    p->count++;
    p->next_ms = now + 1000;
    return false;
}

static bool do_step(knob_ad_t *p, uint32_t now, const knob_ad_ops_t *o, const knob_ad_step_t *s)
{
    switch (s->op) {
    case KNOB_AD_TURN: return do_turn(p, now, o, s->a, s->ms);
    case KNOB_AD_PRESS: return do_press(p, now, o);
    case KNOB_AD_DOUBLE: return do_double(p, now, o);
    case KNOB_AD_LONG: return do_long(p, now, o);
    case KNOB_AD_TOUCH: return do_touch(p, now, o, s->a, s->b);
    case KNOB_AD_WAIT: return (now - p->t0) >= s->ms;
    case KNOB_AD_UNTIL: return (o->cond && o->cond(o->ctx, s->a)) || (now - p->t0) >= s->ms;
    case KNOB_AD_RING_TO: return do_ring_to(p, now, o, s->a);
    case KNOB_AD_SET_TO: return do_set_to(p, now, o, s->a);
    case KNOB_AD_EVENT: if (o->event) o->event(o->ctx, s->a); return true;
    case KNOB_AD_CAPTION: if (o->caption) o->caption(o->ctx, s->text && s->text[0] ? s->text : NULL); return true;
    case KNOB_AD_COUNTDOWN: return do_countdown(p, now, o, s->a);
    default: return true;
    }
}

static void begin_step(knob_ad_t *p, uint32_t now)
{
    p->sub = 0;
    p->count = 0;
    p->t0 = now;
    p->next_ms = now;
}

void knob_ad_init(knob_ad_t *p, const knob_ad_step_t *script)
{
    *p = (knob_ad_t){ 0 };
    p->script = script;
}

void knob_ad_start(knob_ad_t *p, uint32_t now_ms)
{
    p->state = KNOB_AD_RUNNING;
    p->pc = 0;
    begin_step(p, now_ms);
}

static void release(knob_ad_t *p, const knob_ad_ops_t *o)
{
    if (p->btn_down) btn(p, o, false);
    if (p->touch_down) {
        p->touch_down = false;
        if (o->touch) o->touch(o->ctx, p->touch_x, p->touch_y, false);
    }
    fx(o, KNOB_AD_FX_CLEAR, 0, 0);
    if (o->caption) o->caption(o->ctx, NULL);
}

void knob_ad_stop(knob_ad_t *p, const knob_ad_ops_t *ops)
{
    if (p->state == KNOB_AD_RUNNING) release(p, ops);
    p->state = KNOB_AD_OFF;
}

void knob_ad_user_input(knob_ad_t *p, uint32_t now_ms, const knob_ad_ops_t *ops)
{
    p->user_ms = now_ms;
    if (p->state != KNOB_AD_RUNNING) return;
    release(p, ops);
    p->state = KNOB_AD_PAUSED;
}

void knob_ad_tick(knob_ad_t *p, uint32_t now_ms, const knob_ad_ops_t *ops)
{
    if (p->state == KNOB_AD_PAUSED) {
        if (now_ms - p->user_ms >= KNOB_AD_IDLE_RESTART_MS) knob_ad_start(p, now_ms);
        return;
    }
    if (p->state != KNOB_AD_RUNNING || !p->script) return;
    /* A few steps may finish in one tick (events, captions): run until one waits, at most a handful. */
    for (int guard = 0; guard < 8; guard++) {
        const knob_ad_step_t *s = &p->script[p->pc];
        if (s->op == KNOB_AD_END) {
            p->loops++;
            knob_ad_start(p, now_ms);
            return;
        }
        if (!do_step(p, now_ms, ops, s)) return;
        p->steps_run++;
        p->pc++;
        begin_step(p, now_ms);
    }
}
