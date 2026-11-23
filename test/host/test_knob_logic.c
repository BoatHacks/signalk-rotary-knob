/* SPDX-License-Identifier: Apache-2.0
 * Host unit tests for knob_logic (pure C). Build and run with make in this folder, see the Makefile for the Unity path. */
#include <string.h>

#include "knob_logic.h"
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
    TEST_ASSERT_FALSE(knob_boot_show_mode(false, false, false)); /* espOS not up yet */
    TEST_ASSERT_FALSE(knob_boot_show_mode(false, true, false));  /* token flag from a status not read yet does not count */
    TEST_ASSERT_FALSE(knob_boot_show_mode(true, false, false));  /* fresh knob: splash until setup or pairing */
    TEST_ASSERT_TRUE(knob_boot_show_mode(true, true, false));    /* paired, still connecting: last mode early */
    TEST_ASSERT_TRUE(knob_boot_show_mode(true, false, true));    /* online always shows the mode */
    TEST_ASSERT_TRUE(knob_boot_show_mode(false, false, true));
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
    return UNITY_END();
}
