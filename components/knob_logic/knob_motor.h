/* SPDX-License-Identifier: Apache-2.0
 * Motor throttle control (#8), the safety rules as pure code: arming, the throttle steps, the stop on neutral, the limits,
 * and every reason to fall back to neutral. The firmware feeds it the inputs and the clock and sends what it asks for.
 * docs/plans/issue-8-motor-mode.md has the decisions behind each rule. No ESP-IDF, host tested. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNOB_MOTOR_CONFIRM_MS   5000 /* the confirm turn after the arming long press */
#define KNOB_MOTOR_SETTLE_MS    500  /* clicks this soon after arming are the confirm turn's follow-through, ignored */
#define KNOB_MOTOR_SPIN_MS      250  /* clicks closer together than this are one turn: it does not pass neutral */
#define KNOB_MOTOR_KEEPALIVE_MS 1000 /* resend the throttle while out of neutral */
#define KNOB_MOTOR_STALE_MS     3000 /* gear older than this = no motor data */
#define KNOB_MOTOR_MISMATCH_MS  3000 /* gear not matching the throttle's sign this long = fault */
#define KNOB_MOTOR_FAST_MS      50   /* a click less than this after the last one is fast (spike 2026-11-24) */
#define KNOB_MOTOR_SLOW_MS      150  /* more than this is slow */

typedef enum { KNOB_GEAR_UNKNOWN = 0, KNOB_GEAR_FORWARD, KNOB_GEAR_NEUTRAL, KNOB_GEAR_REVERSE, KNOB_GEAR_FAULT } knob_gear_t;
typedef enum { KNOB_MOTOR_CURVE_GENTLE = 0, KNOB_MOTOR_CURVE_NORMAL, KNOB_MOTOR_CURVE_QUICK, KNOB_MOTOR_CURVE_COUNT } knob_motor_curve_t;
typedef enum { KNOB_MOTOR_DISARMED = 0, KNOB_MOTOR_CONFIRM, KNOB_MOTOR_ARMED } knob_motor_state_t;

/* Why the knob refused to arm or disarmed by itself, for the screen. */
typedef enum {
    KNOB_MOTOR_WHY_NONE = 0,
    KNOB_MOTOR_WHY_CONTROL_OFF,   /* allow control is off */
    KNOB_MOTOR_WHY_NO_LINK,
    KNOB_MOTOR_WHY_NO_DATA,       /* the gear is older than KNOB_MOTOR_STALE_MS, or never came */
    KNOB_MOTOR_WHY_NOT_NEUTRAL,   /* arming needs the motor in neutral */
    KNOB_MOTOR_WHY_FAULT,         /* the gear reports Fault, or did not follow the throttle */
    KNOB_MOTOR_WHY_IDLE,          /* armed in neutral without input for the idle timeout */
    KNOB_MOTOR_WHY_USER,          /* long press in neutral */
    KNOB_MOTOR_WHY_CANCELLED,     /* the confirm turn did not come, or went left */
} knob_motor_why_t;

/* What the firmware knows this moment. */
typedef struct {
    uint32_t now_ms;
    bool allow_control;
    bool link_ok;
    knob_gear_t gear;
    uint32_t gear_ms;        /* when the gear last arrived, 0 never */
    uint8_t max_ahead_pct;   /* soft limits, 10..100 */
    uint8_t max_astern_pct;
    uint16_t idle_s;         /* disarm after this long armed in neutral without input, 0 never */
    uint8_t curve;           /* knob_motor_curve_t */
} knob_motor_in_t;

typedef struct {
    knob_motor_state_t state;
    int8_t set_pct;          /* -100..100, what the knob commands */
    uint32_t confirm_ms;     /* when the confirm prompt opened, then when it armed */
    uint32_t last_click_ms;  /* last encoder click while armed (ignored ones too) */
    uint32_t last_input_ms;  /* last input of any kind while armed, for the idle timeout */
    int8_t hold_side;        /* at neutral, reached from +1 (ahead) or -1 (astern): the same turn goes no further */
    bool limit_warning;      /* at a limit: cleared by turning back below it, or lifted with a double press */
    bool limit_lifted;       /* acknowledged with a double press, until the next disarm */
    uint32_t mismatch_ms;    /* since when the gear disagrees with the throttle, 0 = it agrees */
    bool send_due;           /* a new value is waiting to go out */
    uint32_t last_send_ms;
    knob_motor_why_t why;    /* the last refusal or disarm reason */
    uint32_t why_ms;         /* when it happened */
} knob_motor_t;

void knob_motor_init(knob_motor_t *m);

/* Throttle step in percent for one click by the preset and the time since the last click. */
int knob_motor_step(uint8_t curve, uint32_t since_last_ms);

/* Long press. Disarmed: opens the confirm prompt, or refuses (why says why, returns false). Armed out of neutral: throttle 0,
 * stays armed. Armed in neutral: disarm. In the confirm prompt: cancel. Returns true when it did something. */
bool knob_motor_long_press(knob_motor_t *m, const knob_motor_in_t *in);

/* Double press. Returns true when it was used (lifting the limits while the warning shows); false = the UI does its usual
 * thing (the mode ring). Never raises the throttle by itself. */
bool knob_motor_double_press(knob_motor_t *m, const knob_motor_in_t *in);

/* One encoder click, dir +1 right or -1 left. In the confirm prompt a right click arms (at neutral), a left one cancels.
 * Armed: the throttle step with the stop on neutral and the limits. Disarmed: nothing. */
void knob_motor_turn(knob_motor_t *m, const knob_motor_in_t *in, int dir);

/* Call often (every 50 to 100 ms). Times the confirm prompt out and disarms on control off, link loss, stale gear, a fault,
 * a gear that does not follow, or the idle timeout. */
void knob_motor_tick(knob_motor_t *m, const knob_motor_in_t *in);

/* Whether a throttle should go out now: a change, a disarm (0) or the keepalive. Clears the flag and returns the value in
 * percent. Nothing is ever due after init, so a reboot sends nothing. */
bool knob_motor_take_send(knob_motor_t *m, uint32_t now_ms, int8_t *pct);

/* The gear's SignalK value (`"Forward"`, `"Neutral"`, `"Reverse"`, `"Fault"`, quoted or not) to the enum. */
knob_gear_t knob_gear_from_str(const char *v);

#ifdef __cplusplus
}
#endif
