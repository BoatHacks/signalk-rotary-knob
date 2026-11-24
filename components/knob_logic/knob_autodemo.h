/* SPDX-License-Identifier: Apache-2.0
 * Automatic demo (#10): a step player that drives the knob's UI through a script, the way a person would. Pure C, no
 * ESP-IDF, host tested.
 *
 * The player emits raw inputs (encoder clicks, button edges, touches) through callbacks, so the UI's own gesture timing
 * applies: a single press resolves 300 ms after the release, a long press fires after 750 ms, two releases close together
 * are a double press. It also tells an overlay what to draw for every input, sets a caption, asks the firmware about the
 * state (conditions, the mode ring, the settings list) and fires simulation events.
 *
 * Call knob_ad_tick() often (every few ms) with the time. Real input calls knob_ad_user_input(): the script pauses and
 * starts again from the top after KNOB_AD_IDLE_RESTART_MS without input. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNOB_AD_IDLE_RESTART_MS 60000u
#define KNOB_AD_CLICK_GAP_MS    220u   /* between two encoder clicks of one turn */
#define KNOB_AD_PRESS_MS        90u    /* a short press, down to up */
#define KNOB_AD_LONG_MS         950u   /* a long press: more than the UI's 750 ms */
#define KNOB_AD_DOUBLE_GAP_MS   110u   /* up to down between the two presses of a double press */
#define KNOB_AD_SETTLE_MS       420u   /* after a press: the UI's single press resolves 300 ms after the release */

typedef enum {
    KNOB_AD_TURN,        /* a: clicks, + clockwise. ms: gap between clicks (0 = KNOB_AD_CLICK_GAP_MS) */
    KNOB_AD_PRESS,       /* a short press */
    KNOB_AD_DOUBLE,      /* a double press */
    KNOB_AD_LONG,        /* a long press */
    KNOB_AD_TOUCH,       /* a: x, b: y (0..239) */
    KNOB_AD_WAIT,        /* ms */
    KNOB_AD_UNTIL,       /* a: condition id, ms: timeout. Moves on when the condition holds, or after the timeout */
    KNOB_AD_RING_TO,     /* a: mode, b: -1 turns counter-clockwise. Double press, turn until the focus is on it, press */
    KNOB_AD_SET_TO,      /* a: settings item. Turn the settings list until it is focused */
    KNOB_AD_EVENT,       /* a: event id, for the simulation */
    KNOB_AD_CAPTION,     /* text, NULL or "" hides it */
    KNOB_AD_COUNTDOWN,   /* a: from (3 shows 3, 2, 1, a second each) */
    KNOB_AD_END          /* the end: start again from the top */
} knob_ad_op_t;

typedef struct {
    uint8_t op;          /* knob_ad_op_t */
    int16_t a, b;
    uint16_t ms;
    const char *text;
} knob_ad_step_t;

/* What the overlay draws (knob_ad_ops_t.show). */
typedef enum {
    KNOB_AD_FX_TURN,     /* a: direction (+1 / -1), b: clicks in this turn so far */
    KNOB_AD_FX_PRESS,
    KNOB_AD_FX_DOUBLE,
    KNOB_AD_FX_LONG,     /* the hold starts: a ring fills for KNOB_AD_LONG_MS */
    KNOB_AD_FX_TOUCH,    /* a: x, b: y */
    KNOB_AD_FX_COUNT,    /* a: the countdown digit, 0 hides it */
    KNOB_AD_FX_CLEAR     /* the script paused or stopped: hide everything */
} knob_ad_fx_t;

/* What the player asks the firmware (knob_ad_ops_t.query). */
typedef enum {
    KNOB_AD_Q_MODE,      /* the active mode */
    KNOB_AD_Q_RING_OPEN, /* 1 while the mode ring is open */
    KNOB_AD_Q_RING_FOCUS,/* the mode the ring's focus is on */
    KNOB_AD_Q_SET_INDEX  /* the focused settings item */
} knob_ad_query_t;

typedef struct {
    void (*turn)(void *ctx, int dir);
    void (*button)(void *ctx, bool down);
    void (*touch)(void *ctx, int16_t x, int16_t y, bool down);
    void (*show)(void *ctx, knob_ad_fx_t fx, int a, int b);
    void (*caption)(void *ctx, const char *text);
    bool (*cond)(void *ctx, int id);
    void (*event)(void *ctx, int id);
    int (*query)(void *ctx, knob_ad_query_t q);
    void *ctx;
} knob_ad_ops_t;

typedef enum { KNOB_AD_OFF = 0, KNOB_AD_RUNNING, KNOB_AD_PAUSED } knob_ad_state_t;

typedef struct {
    const knob_ad_step_t *script;
    uint8_t state;        /* knob_ad_state_t */
    uint16_t pc;          /* the step */
    uint8_t sub;          /* where in the step */
    int16_t count;        /* clicks or digits done in this step */
    uint32_t t0;          /* when the current wait started */
    uint32_t next_ms;     /* the next micro-action is due */
    uint32_t user_ms;     /* the last real input */
    bool btn_down, touch_down;
    int16_t touch_x, touch_y;
    uint32_t steps_run;   /* for tests and the log */
    uint32_t loops;
} knob_ad_t;

void knob_ad_init(knob_ad_t *p, const knob_ad_step_t *script);
void knob_ad_start(knob_ad_t *p, uint32_t now_ms);                       /* from the top */
void knob_ad_stop(knob_ad_t *p, const knob_ad_ops_t *ops);               /* releases a held button, clears the overlay */
void knob_ad_user_input(knob_ad_t *p, uint32_t now_ms, const knob_ad_ops_t *ops); /* pauses a running script */
void knob_ad_tick(knob_ad_t *p, uint32_t now_ms, const knob_ad_ops_t *ops);
static inline bool knob_ad_running(const knob_ad_t *p) { return p->state == KNOB_AD_RUNNING; }
static inline bool knob_ad_active(const knob_ad_t *p) { return p->state != KNOB_AD_OFF; }

/* The script the knob runs (#10, docs/plans/issue-10-auto-demo.md). Conditions and events below are what the firmware
 * implements for it. */
enum {
    KNOB_AD_COND_BOOT_DONE = 1,   /* the simulated boot reached the mode screen */
    KNOB_AD_COND_ENGAGED,
    KNOB_AD_COND_HEADING_SETTLED, /* engaged and within 1 degree of the target */
    KNOB_AD_COND_ANCHOR_DROPPED,
    KNOB_AD_COND_ALARM,           /* an alarm is active */
    KNOB_AD_COND_NO_ALARM
};
enum {
    KNOB_AD_EV_RESET = 1,         /* the simulation back to the start state, the autopilot screen */
    KNOB_AD_EV_BOOT,              /* show the splash with the boot phases */
    KNOB_AD_EV_DRAG               /* the boat starts to swing wider than the watch circle */
};
extern const knob_ad_step_t knob_ad_script[];

#ifdef __cplusplus
}
#endif
