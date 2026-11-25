/* SPDX-License-Identifier: Apache-2.0 */
#include "knob_motor.h"

#include <ctype.h>
#include <string.h>

/* Percent per click for slow, normal and fast clicks (owner, 2026-11-24). */
static const uint8_t s_steps[KNOB_MOTOR_CURVE_COUNT][3] = {
    { 1, 1, 2 },   /* gentle */
    { 1, 2, 5 },   /* normal, the default */
    { 1, 3, 10 },  /* quick */
};

void knob_motor_init(knob_motor_t *m)
{
    memset(m, 0, sizeof(*m));
}

int knob_motor_step(uint8_t curve, uint32_t since_last_ms)
{
    if (curve >= KNOB_MOTOR_CURVE_COUNT) curve = KNOB_MOTOR_CURVE_NORMAL;
    if (since_last_ms > KNOB_MOTOR_SLOW_MS) return s_steps[curve][0];
    if (since_last_ms < KNOB_MOTOR_FAST_MS) return s_steps[curve][2];
    return s_steps[curve][1];
}

static void note(knob_motor_t *m, uint32_t now, knob_motor_why_t why)
{
    m->why = why;
    m->why_ms = now;
}

/* Back to disarmed. From armed the throttle goes to 0 and that 0 is sent once (when the link still allows it). */
static void disarm(knob_motor_t *m, uint32_t now, knob_motor_why_t why)
{
    if (m->state == KNOB_MOTOR_ARMED) {
        m->set_pct = 0;
        m->send_due = true;
    }
    m->state = KNOB_MOTOR_DISARMED;
    m->hold_side = 0;
    m->limit_warning = false;
    m->limit_lifted = false;
    m->mismatch_ms = 0;
    note(m, now, why);
}

static bool data_ok(const knob_motor_in_t *in)
{
    return in->gear_ms != 0 && in->now_ms - in->gear_ms <= KNOB_MOTOR_STALE_MS;
}

/* The first reason the knob must not be (or stay) in control, NONE when all is well. Not the gear's position. */
static knob_motor_why_t blocked(const knob_motor_in_t *in)
{
    if (!in->allow_control) return KNOB_MOTOR_WHY_CONTROL_OFF;
    if (!in->link_ok) return KNOB_MOTOR_WHY_NO_LINK;
    if (!data_ok(in)) return KNOB_MOTOR_WHY_NO_DATA;
    if (in->gear == KNOB_GEAR_FAULT) return KNOB_MOTOR_WHY_FAULT;
    return KNOB_MOTOR_WHY_NONE;
}

bool knob_motor_long_press(knob_motor_t *m, const knob_motor_in_t *in)
{
    uint32_t now = in->now_ms;
    switch (m->state) {
    case KNOB_MOTOR_CONFIRM:
        m->state = KNOB_MOTOR_DISARMED;
        note(m, now, KNOB_MOTOR_WHY_CANCELLED);
        return true;
    case KNOB_MOTOR_DISARMED: {
        knob_motor_why_t why = blocked(in);
        if (why == KNOB_MOTOR_WHY_NONE && in->gear != KNOB_GEAR_NEUTRAL) why = KNOB_MOTOR_WHY_NOT_NEUTRAL;
        if (why != KNOB_MOTOR_WHY_NONE) {
            note(m, now, why);
            return false;
        }
        m->state = KNOB_MOTOR_CONFIRM;
        m->confirm_ms = now;
        note(m, now, KNOB_MOTOR_WHY_NONE);
        return true;
    }
    case KNOB_MOTOR_ARMED:
    default:
        m->last_input_ms = now;
        if (m->set_pct != 0) {
            m->set_pct = 0;      /* neutral at once, stays armed; the next turn may go either way */
            m->hold_side = 0;
            m->send_due = true;
        } else {
            disarm(m, now, KNOB_MOTOR_WHY_USER);
        }
        return true;
    }
}

bool knob_motor_double_press(knob_motor_t *m, const knob_motor_in_t *in)
{
    if (m->state != KNOB_MOTOR_ARMED || !m->limit_warning) return false;
    m->limit_warning = false;
    m->limit_lifted = true;      /* both directions, until the next disarm; the throttle stays where it is */
    m->last_input_ms = in->now_ms;
    return true;
}

void knob_motor_turn(knob_motor_t *m, const knob_motor_in_t *in, int dir)
{
    uint32_t now = in->now_ms;
    if (dir == 0) return;
    dir = dir > 0 ? 1 : -1;
    if (m->state == KNOB_MOTOR_CONFIRM) {
        if (dir > 0) {
            m->state = KNOB_MOTOR_ARMED;
            m->set_pct = 0;
            m->hold_side = 0;
            m->limit_warning = m->limit_lifted = false;
            m->mismatch_ms = 0;
            m->confirm_ms = now;
            m->last_click_ms = now;
            m->last_input_ms = now;
            m->send_due = true;   /* the motor hears 0 from the knob from the start */
            note(m, now, KNOB_MOTOR_WHY_NONE);
        } else {
            m->state = KNOB_MOTOR_DISARMED;
            note(m, now, KNOB_MOTOR_WHY_CANCELLED);
        }
        return;
    }
    if (m->state != KNOB_MOTOR_ARMED) return;

    if (now - m->confirm_ms < KNOB_MOTOR_SETTLE_MS) {   /* a spin that confirmed must not run the throttle up */
        m->last_click_ms = now;
        return;
    }
    uint32_t since = now - m->last_click_ms;
    m->last_click_ms = now;
    m->last_input_ms = now;
    int cur = m->set_pct;

    /* The stop on neutral (owner, 2026-11-25): a spin that reaches 0 ends there, the rest of the same spin is ignored. The
     * next turn goes on to the other side, back to the side it came from works at once. */
    if (cur == 0 && m->hold_side != 0) {
        if (dir == -m->hold_side && since < KNOB_MOTOR_SPIN_MS) return;
        m->hold_side = 0;
    }

    int nv = cur + dir * knob_motor_step(in->curve, since);
    if (cur > 0 && nv <= 0) { nv = 0; m->hold_side = 1; }
    else if (cur < 0 && nv >= 0) { nv = 0; m->hold_side = -1; }

    if (!m->limit_lifted) {
        int ahead = in->max_ahead_pct > 100 ? 100 : in->max_ahead_pct;
        int astern = in->max_astern_pct > 100 ? 100 : in->max_astern_pct;
        if (nv > ahead) { nv = ahead; m->limit_warning = true; }
        else if (nv < -astern) { nv = -astern; m->limit_warning = true; }
        else if (nv < ahead && nv > -astern) m->limit_warning = false;   /* turned back below the limit (owner, 2026-11-24) */
    }
    if (nv > 100) nv = 100;
    if (nv < -100) nv = -100;
    if (nv != cur) {
        m->set_pct = (int8_t)nv;
        m->send_due = true;
    }
}

void knob_motor_tick(knob_motor_t *m, const knob_motor_in_t *in)
{
    uint32_t now = in->now_ms;
    if (m->state == KNOB_MOTOR_CONFIRM) {
        knob_motor_why_t why = blocked(in);
        if (why != KNOB_MOTOR_WHY_NONE || in->gear != KNOB_GEAR_NEUTRAL) {
            m->state = KNOB_MOTOR_DISARMED;
            note(m, now, why != KNOB_MOTOR_WHY_NONE ? why : KNOB_MOTOR_WHY_NOT_NEUTRAL);
        } else if (now - m->confirm_ms >= KNOB_MOTOR_CONFIRM_MS) {
            m->state = KNOB_MOTOR_DISARMED;
            note(m, now, KNOB_MOTOR_WHY_CANCELLED);
        }
        return;
    }
    if (m->state != KNOB_MOTOR_ARMED) return;

    knob_motor_why_t why = blocked(in);
    if (why != KNOB_MOTOR_WHY_NONE) { disarm(m, now, why); return; }

    knob_gear_t want = m->set_pct > 0 ? KNOB_GEAR_FORWARD : m->set_pct < 0 ? KNOB_GEAR_REVERSE : KNOB_GEAR_NEUTRAL;
    if (in->gear != want) {
        if (m->mismatch_ms == 0) m->mismatch_ms = now ? now : 1;
        else if (now - m->mismatch_ms > KNOB_MOTOR_MISMATCH_MS) { disarm(m, now, KNOB_MOTOR_WHY_FAULT); return; }
    } else {
        m->mismatch_ms = 0;
    }

    if (in->idle_s && m->set_pct == 0 && now - m->last_input_ms >= (uint32_t)in->idle_s * 1000u) {
        disarm(m, now, KNOB_MOTOR_WHY_IDLE);
    }
}

bool knob_motor_take_send(knob_motor_t *m, uint32_t now_ms, int8_t *pct)
{
    bool due = m->send_due ||
               (m->state == KNOB_MOTOR_ARMED && m->set_pct != 0 && now_ms - m->last_send_ms >= KNOB_MOTOR_KEEPALIVE_MS);
    if (!due) return false;
    m->send_due = false;
    m->last_send_ms = now_ms;
    *pct = m->set_pct;
    return true;
}

static bool word_is(const char *v, const char *w)
{
    size_t n = strlen(w);
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)v[i]) != w[i]) return false;
    }
    return v[n] == '\0' || v[n] == '"';
}

knob_gear_t knob_gear_from_str(const char *v)
{
    if (!v) return KNOB_GEAR_UNKNOWN;
    while (*v == ' ' || *v == '"') v++;
    if (word_is(v, "forward")) return KNOB_GEAR_FORWARD;
    if (word_is(v, "neutral")) return KNOB_GEAR_NEUTRAL;
    if (word_is(v, "reverse")) return KNOB_GEAR_REVERSE;
    if (word_is(v, "fault")) return KNOB_GEAR_FAULT;
    return KNOB_GEAR_UNKNOWN;
}
