/* SPDX-License-Identifier: Apache-2.0
 * Host unit tests for knob_logic (pure C). Build and run with make in this folder, see the Makefile for the Unity path. */
#include <string.h>

#include "knob_logic.h"
#include "knob_demo.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- JSON value helpers ---- */

static void test_parse_num_plain_numbers(void)
{
    double d = 0;
    TEST_ASSERT_TRUE(knob_parse_num("3.71685", &d));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 3.71685f, (float)d);
    TEST_ASSERT_TRUE(knob_parse_num("  -12", &d));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -12.0f, (float)d);
    TEST_ASSERT_TRUE(knob_parse_num("0", &d));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, (float)d);
}

static void test_parse_num_rejects_non_numbers(void)
{
    double d = 42;
    TEST_ASSERT_FALSE(knob_parse_num("null", &d));
    TEST_ASSERT_FALSE(knob_parse_num("\"night\"", &d));
    TEST_ASSERT_FALSE(knob_parse_num("{\"a\":1}", &d));
    TEST_ASSERT_FALSE(knob_parse_num("", &d));
    TEST_ASSERT_FALSE(knob_parse_num(NULL, &d));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 42.0f, (float)d); /* untouched on failure */
}

static void test_parse_str(void)
{
    char s[16];
    TEST_ASSERT_TRUE(knob_parse_str("\"night\"", s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("night", s);
    TEST_ASSERT_TRUE(knob_parse_str("\"abcdef\"", s, 4)); /* truncated, always terminated */
    TEST_ASSERT_EQUAL_STRING("abc", s);
    TEST_ASSERT_TRUE(knob_parse_str("\"abc", s, sizeof(s))); /* unterminated: no read past the NUL */
    TEST_ASSERT_EQUAL_STRING("abc", s);
    TEST_ASSERT_FALSE(knob_parse_str("night", s, sizeof(s)));
    TEST_ASSERT_FALSE(knob_parse_str("null", s, sizeof(s)));
    TEST_ASSERT_FALSE(knob_parse_str(NULL, s, sizeof(s)));
}

static void test_obj_num_position(void)
{
    const char *pos = "{\"latitude\":54.123456,\"longitude\":10.5}";
    double la = 0, lo = 0;
    TEST_ASSERT_TRUE(knob_obj_num(pos, "latitude", &la));
    TEST_ASSERT_TRUE(knob_obj_num(pos, "longitude", &lo));
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 54.123456f, (float)la);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 10.5f, (float)lo);
}

static void test_obj_num_spaces_missing_and_wrong_type(void)
{
    double d = 0;
    TEST_ASSERT_TRUE(knob_obj_num("{ \"latitude\" : 1.5 }", "latitude", &d));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, (float)d);
    TEST_ASSERT_FALSE(knob_obj_num("{\"latitude\":1.5}", "longitude", &d));
    TEST_ASSERT_FALSE(knob_obj_num("{\"latitude\":\"north\"}", "latitude", &d));
    TEST_ASSERT_FALSE(knob_obj_num("{\"latitude\":null}", "latitude", &d));
    TEST_ASSERT_FALSE(knob_obj_num(NULL, "latitude", &d));
}

static void test_obj_str_alarm_state(void)
{
    char st[12];
    TEST_ASSERT_TRUE(knob_obj_str("{\"state\":\"alarm\",\"method\":[\"visual\",\"sound\"]}", "state", st, sizeof(st)));
    TEST_ASSERT_EQUAL_STRING("alarm", st);
    TEST_ASSERT_TRUE(knob_obj_str("{\"state\":\"emergency\"}", "state", st, sizeof(st)));
    TEST_ASSERT_EQUAL_STRING("emergency", st);
    TEST_ASSERT_FALSE(knob_obj_str("{\"state\":5}", "state", st, sizeof(st)));
    TEST_ASSERT_FALSE(knob_obj_str("{\"message\":\"x\"}", "state", st, sizeof(st)));
}

/* ---- staleness ---- */

static void test_stale_rules(void)
{
    TEST_ASSERT_FALSE(knob_stale_by(100000, 0, 0));      /* timeout 0: never stale, even if never received */
    TEST_ASSERT_FALSE(knob_stale_by(100000, 1000, 0));
    TEST_ASSERT_TRUE(knob_stale_by(100000, 0, 5000));    /* never received */
    TEST_ASSERT_FALSE(knob_stale_by(100000, 96000, 5000));
    TEST_ASSERT_FALSE(knob_stale_by(100000, 95000, 5000)); /* exactly the timeout is still fresh */
    TEST_ASSERT_TRUE(knob_stale_by(100000, 94999, 5000));
}

/* ---- steering ---- */

static void test_course_error_wraps(void)
{
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, knob_course_error(10.0f, 350.0f));  /* target to the right across north */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -20.0f, knob_course_error(350.0f, 10.0f)); /* to the left */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, knob_course_error(90.0f, 90.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -179.0f, knob_course_error(181.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 179.0f, knob_course_error(179.0f, 0.0f));
}

static void test_steer_colours_and_hysteresis(void)
{
    bool on = false;
    TEST_ASSERT_EQUAL(KNOB_STEER_STBD, knob_steer_step(2.5f, &on, 2.0f, 3.0f));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_EQUAL(KNOB_STEER_ON_COURSE, knob_steer_step(1.9f, &on, 2.0f, 3.0f));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_EQUAL(KNOB_STEER_ON_COURSE, knob_steer_step(2.9f, &on, 2.0f, 3.0f)); /* stays on course inside the leave band */
    TEST_ASSERT_EQUAL(KNOB_STEER_ON_COURSE, knob_steer_step(-2.9f, &on, 2.0f, 3.0f));
    TEST_ASSERT_EQUAL(KNOB_STEER_STBD, knob_steer_step(3.1f, &on, 2.0f, 3.0f));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_EQUAL(KNOB_STEER_STBD, knob_steer_step(2.5f, &on, 2.0f, 3.0f)); /* does not re-enter at 2.5 */
    TEST_ASSERT_EQUAL(KNOB_STEER_PORT, knob_steer_step(-3.1f, &on, 2.0f, 3.0f));
    TEST_ASSERT_EQUAL(KNOB_STEER_PORT, knob_steer_step(-45.0f, &on, 2.0f, 3.0f));
}

static void test_night_fade(void)
{
    TEST_ASSERT_EQUAL_UINT32(1000, knob_fade_permille(0, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, knob_fade_permille(1999, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(1000, knob_fade_permille(2000, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(500, knob_fade_permille(2500, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(1, knob_fade_permille(2999, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, knob_fade_permille(3000, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, knob_fade_permille(60000, 2000, 1000));
    TEST_ASSERT_EQUAL_UINT32(0, knob_fade_permille(2000, 2000, 0)); /* no fade time: off right after the hold */
}

/* ---- GPS lost ---- */

static void test_gps_lost(void)
{
    /* armed, not in the boot grace, position fresh, fix ok */
    TEST_ASSERT_FALSE(knob_gps_lost(true, 100000, 45000, true, 99000, 30000, true, true));
    TEST_ASSERT_FALSE(knob_gps_lost(false, 100000, 45000, false, 0, 30000, true, false)); /* not armed: never */
    TEST_ASSERT_FALSE(knob_gps_lost(true, 10000, 45000, false, 0, 30000, true, false));   /* boot grace */
    TEST_ASSERT_TRUE(knob_gps_lost(true, 100000, 45000, true, 60000, 30000, true, true)); /* position stale */
    TEST_ASSERT_TRUE(knob_gps_lost(true, 100000, 45000, false, 0, 30000, true, true));    /* never had a position */
    TEST_ASSERT_TRUE(knob_gps_lost(true, 100000, 45000, true, 99000, 30000, true, false)); /* fix lost */
    TEST_ASSERT_FALSE(knob_gps_lost(true, 100000, 45000, true, 99000, 30000, false, false)); /* fix state unknown */
}

/* ---- Boot ---- */

static void test_boot_show_mode(void)
{
    bool verified = false;
    TEST_ASSERT_FALSE(knob_boot_show_mode(false, &verified)); /* Wi-Fi, server search, token check: splash */
    TEST_ASSERT_FALSE(knob_boot_show_mode(false, &verified));
    TEST_ASSERT_TRUE(knob_boot_show_mode(true, &verified));   /* token verified: the mode screen */
    TEST_ASSERT_TRUE(knob_boot_show_mode(false, &verified));  /* a later drop keeps the mode screen (LINK LOST) */
    TEST_ASSERT_TRUE(verified);
}

static void test_boot_mode(void)
{
    /* four modes, HID is 3 */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(0, 4, 3));
    TEST_ASSERT_EQUAL_INT(1, knob_boot_mode(1, 4, 3));
    TEST_ASSERT_EQUAL_INT(2, knob_boot_mode(2, 4, 3));
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(3, 4, 3));  /* HID is not a non-HID mode */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(4, 4, 3));  /* out of range, for example from an older firmware */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(-1, 4, 3)); /* nothing stored */
}

static void test_boot_hid(void)
{
    TEST_ASSERT_TRUE(knob_boot_hid(true, true, 3000, 10000, false));
    TEST_ASSERT_FALSE(knob_boot_hid(false, true, 3000, 10000, false)); /* HID was not the last mode */
    TEST_ASSERT_FALSE(knob_boot_hid(true, false, 3000, 10000, false)); /* no host: last non-HID mode */
    TEST_ASSERT_FALSE(knob_boot_hid(true, true, 10000, 10000, false)); /* window closed */
    TEST_ASSERT_FALSE(knob_boot_hid(true, true, 3000, 10000, true));   /* the user picked a mode meanwhile */
}

/* ---- Mode API and allow control ---- */

static void test_mode_names(void)
{
    TEST_ASSERT_EQUAL_INT(0, knob_mode_from_name("autopilot"));
    TEST_ASSERT_EQUAL_INT(1, knob_mode_from_name("lights"));
    TEST_ASSERT_EQUAL_INT(2, knob_mode_from_name("anchor"));
    TEST_ASSERT_EQUAL_INT(3, knob_mode_from_name("hid"));
    TEST_ASSERT_EQUAL_INT(-1, knob_mode_from_name("Anchor"));
    TEST_ASSERT_EQUAL_INT(-1, knob_mode_from_name(""));
    TEST_ASSERT_EQUAL_INT(-1, knob_mode_from_name(NULL));
    TEST_ASSERT_EQUAL_STRING("lights", knob_mode_name(1));
    TEST_ASSERT_EQUAL_STRING("settings", knob_mode_name(4));
    TEST_ASSERT_EQUAL_STRING("unknown", knob_mode_name(5));
    TEST_ASSERT_EQUAL_STRING("unknown", knob_mode_name(-1));
}

static void test_mode_request(void)
{
    int m = -1;
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_OK, knob_mode_request("{\"mode\":\"anchor\"}", false, &m));
    TEST_ASSERT_EQUAL_INT(2, m);
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_OK, knob_mode_request("{ \"mode\" : \"lights\" }", false, &m));
    TEST_ASSERT_EQUAL_INT(1, m);
    m = -1;
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_NO_HOST, knob_mode_request("{\"mode\":\"hid\"}", false, &m));
    TEST_ASSERT_EQUAL_INT(-1, m); /* not set on a refusal */
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_OK, knob_mode_request("{\"mode\":\"hid\"}", true, &m));
    TEST_ASSERT_EQUAL_INT(3, m);
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request("{\"mode\":\"engage\"}", true, &m));
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request("{\"mode\":\"settings\"}", true, &m));
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request("{\"mode\":2}", true, &m));
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request("{}", true, &m));
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request("", true, &m));
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_REQ_BAD, knob_mode_request(NULL, true, &m));
}

static void test_allow_control(void)
{
    TEST_ASSERT_FALSE(knob_allow_control(false, -1)); /* stored off, no override: a fresh knob */
    TEST_ASSERT_TRUE(knob_allow_control(true, -1));
    TEST_ASSERT_TRUE(knob_allow_control(false, 1));   /* serial override on for this session */
    TEST_ASSERT_FALSE(knob_allow_control(true, 0));   /* serial override off beats the stored on */
}

/* ---- Settings screen ---- */

static void test_list_wrap(void)
{
    TEST_ASSERT_EQUAL_INT(1, knob_list_wrap(0, 1, 13));
    TEST_ASSERT_EQUAL_INT(0, knob_list_wrap(12, 1, 13));  /* past the end to the first */
    TEST_ASSERT_EQUAL_INT(12, knob_list_wrap(0, -1, 13)); /* before the first to the last */
    TEST_ASSERT_EQUAL_INT(3, knob_list_wrap(12, 4, 13));
    TEST_ASSERT_EQUAL_INT(0, knob_list_wrap(5, 1, 0));    /* empty list */
}

static void test_setting_steps(void)
{
    TEST_ASSERT_EQUAL_INT(90, knob_setting_step(KNOB_SET_BRIGHTNESS, 80, 1));
    TEST_ASSERT_EQUAL_INT(70, knob_setting_step(KNOB_SET_BRIGHTNESS, 80, -1));
    TEST_ASSERT_EQUAL_INT(100, knob_setting_step(KNOB_SET_BRIGHTNESS, 100, 1)); /* clamped */
    TEST_ASSERT_EQUAL_INT(10, knob_setting_step(KNOB_SET_BRIGHTNESS, 10, -3));
    TEST_ASSERT_EQUAL_INT(30, knob_setting_step(KNOB_SET_DIM_AFTER, 25, 1));    /* between 20 and 30: up to 30 */
    TEST_ASSERT_EQUAL_INT(20, knob_setting_step(KNOB_SET_DIM_AFTER, 25, -1));   /* and down to 20 */
    TEST_ASSERT_EQUAL_INT(45, knob_setting_step(KNOB_SET_DIM_AFTER, 25, 2));
    TEST_ASSERT_EQUAL_INT(5, knob_setting_step(KNOB_SET_DIM_AFTER, 1, -1));     /* below the list */
    TEST_ASSERT_EQUAL_INT(5, knob_setting_step(KNOB_SET_DIM_AFTER, 1, 1));      /* below the list, up: the first step */
    TEST_ASSERT_EQUAL_INT(3600, knob_setting_step(KNOB_SET_DIM_AFTER, 9999, 1));/* above the list */
    TEST_ASSERT_EQUAL_INT(1, knob_setting_step(KNOB_SET_OFF_AFTER, 0, 1));
    TEST_ASSERT_EQUAL_INT(2, knob_setting_step(KNOB_SET_NIGHT, 1, 1));
    TEST_ASSERT_EQUAL_INT(2, knob_setting_step(KNOB_SET_NIGHT, 2, 1));          /* choices clamp too */
    TEST_ASSERT_EQUAL_INT(0, knob_setting_step(KNOB_SET_LEDS, 1, -5));
    TEST_ASSERT_EQUAL_INT(7, knob_setting_step(KNOB_SET_RESET, 7, 1));          /* not a value */
}

static void test_setting_format(void)
{
    char b[24];
    knob_setting_format(KNOB_SET_BRIGHTNESS, 80, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("80 %", b);
    knob_setting_format(KNOB_SET_OFF_AFTER, 0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("NEVER", b);
    knob_setting_format(KNOB_SET_OFF_AFTER, 10, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("10 min", b);
    knob_setting_format(KNOB_SET_NIGHT, 0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("SERVER", b);
    knob_setting_format(KNOB_SET_HEADING, 1, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("MAGNETIC", b);
    knob_setting_format(KNOB_SET_LEDS, 5, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("?", b);
    knob_setting_format(KNOB_SET_DEVICE_INFO, 0, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("", b);
    TEST_ASSERT_NULL(knob_setting_desc(KNOB_SET_COUNT));
}

static void test_confirm_turn(void)
{
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_WAITING, knob_confirm_step(1000, 2000, 5000, KNOB_CONFIRM_EV_TICK, 0));
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_DONE, knob_confirm_step(1000, 2000, 5000, KNOB_CONFIRM_EV_TURN, 1));
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_WAITING, knob_confirm_step(1000, 2000, 5000, KNOB_CONFIRM_EV_TURN, -1)); /* left: ignored */
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_CANCELLED, knob_confirm_step(1000, 2000, 5000, KNOB_CONFIRM_EV_PRESS, 0));
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_CANCELLED, knob_confirm_step(1000, 6000, 5000, KNOB_CONFIRM_EV_TURN, 1)); /* too late */
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_CANCELLED, knob_confirm_step(1000, 6000, 5000, KNOB_CONFIRM_EV_TICK, 0));
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_DONE, knob_confirm_step(0xFFFFF000u, 0x00000100u, 5000, KNOB_CONFIRM_EV_TURN, 1)); /* clock wrap */
}

/* ---- Demo mode simulation ---- */

static void test_demo_autopilot(void)
{
    knob_demo_t d;
    knob_demo_init(&d, 0, 35);
    knob_demo_step(&d, 1000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, d.heading_deg, d.target_deg); /* standby: the target follows the heading */
    knob_demo_engage(&d, true);
    TEST_ASSERT_TRUE(d.engaged);
    knob_demo_set_target(&d, d.heading_deg + 30.0f);
    float before = d.heading_deg;
    knob_demo_step(&d, 2000);                                      /* one second: at most about 3 degrees */
    float moved = knob_course_error(d.heading_deg, before);
    TEST_ASSERT_TRUE(moved > 2.0f && moved < 3.5f);
    TEST_ASSERT_TRUE(d.rudder_deg > 0.0f);                         /* turning to starboard */
    for (uint32_t t = 3000; t < 30000; t += 1000) knob_demo_step(&d, t);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, knob_course_error(d.target_deg, d.heading_deg)); /* arrived */
    knob_demo_set_target(&d, 365.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, d.target_deg);          /* wrapped */
    knob_demo_engage(&d, false);
    TEST_ASSERT_FALSE(d.engaged);
}

static void test_demo_anchor(void)
{
    knob_demo_t d;
    knob_demo_init(&d, 0, 35);
    TEST_ASSERT_FALSE(knob_demo_dragging(&d));
    knob_demo_drop(&d, 35);
    for (uint32_t t = 1000; t < 120000; t += 1000) {
        knob_demo_step(&d, t);
        TEST_ASSERT_FALSE(knob_demo_dragging(&d));                 /* swing 22 m inside 35 m */
        TEST_ASSERT_TRUE(d.anchor_dist_m <= d.swing_m + 0.01f);
    }
    knob_demo_set_radius(&d, 10);                                   /* below the swing: drag alarm */
    TEST_ASSERT_TRUE(knob_demo_dragging(&d));
    knob_demo_set_radius(&d, 0);                                    /* 0 is ignored */
    TEST_ASSERT_EQUAL_FLOAT(10.0f, d.radius_m);
    knob_demo_raise(&d);
    TEST_ASSERT_FALSE(knob_demo_dragging(&d));
    knob_demo_set_light(&d, 1, true);
    TEST_ASSERT_TRUE(d.light[1]);
    knob_demo_set_light(&d, 9, true);                               /* out of range: ignored */
    uint32_t big = 0xFFFFFF00u;
    knob_demo_init(&d, big, 35);
    knob_demo_step(&d, big + 100000u);                              /* a long gap moves at most 5 s worth */
    TEST_ASSERT_TRUE(d.depth_m > 5.9f && d.depth_m < 6.9f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_num_plain_numbers);
    RUN_TEST(test_parse_num_rejects_non_numbers);
    RUN_TEST(test_parse_str);
    RUN_TEST(test_obj_num_position);
    RUN_TEST(test_obj_num_spaces_missing_and_wrong_type);
    RUN_TEST(test_obj_str_alarm_state);
    RUN_TEST(test_stale_rules);
    RUN_TEST(test_course_error_wraps);
    RUN_TEST(test_steer_colours_and_hysteresis);
    RUN_TEST(test_night_fade);
    RUN_TEST(test_gps_lost);
    RUN_TEST(test_boot_show_mode);
    RUN_TEST(test_boot_mode);
    RUN_TEST(test_boot_hid);
    RUN_TEST(test_mode_names);
    RUN_TEST(test_mode_request);
    RUN_TEST(test_allow_control);
    RUN_TEST(test_list_wrap);
    RUN_TEST(test_setting_steps);
    RUN_TEST(test_setting_format);
    RUN_TEST(test_confirm_turn);
    RUN_TEST(test_demo_autopilot);
    RUN_TEST(test_demo_anchor);
    return UNITY_END();
}
