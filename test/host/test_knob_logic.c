/* SPDX-License-Identifier: Apache-2.0
 * Host unit tests for knob_logic (pure C). Build and run with make in this folder, see the Makefile for the Unity path. */
#include <string.h>

#include "knob_logic.h"
#include "knob_demo.h"
#include "knob_motor.h"
#include "knob_autodemo.h"
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
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(0, 4, 3, 9));
    TEST_ASSERT_EQUAL_INT(1, knob_boot_mode(1, 4, 3, 9));
    TEST_ASSERT_EQUAL_INT(2, knob_boot_mode(2, 4, 3, 9));
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(3, 4, 3, 9));  /* HID is not a non-HID mode */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(4, 4, 3, 9));  /* out of range, for example from an older firmware */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(-1, 4, 3, 9)); /* nothing stored */
    TEST_ASSERT_EQUAL_INT(0, knob_boot_mode(4, 6, 3, 4));  /* settings is never restored */
    TEST_ASSERT_EQUAL_INT(5, knob_boot_mode(5, 6, 3, 4));  /* music, after settings in the numbering */
    TEST_ASSERT_EQUAL_INT(6, knob_boot_mode(6, 7, 3, 4));  /* motor, restored disarmed */
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
    TEST_ASSERT_EQUAL_STRING("music", knob_mode_name(5));
    TEST_ASSERT_EQUAL_INT(5, knob_mode_from_name("music"));
    TEST_ASSERT_EQUAL_STRING("motor", knob_mode_name(6));
    TEST_ASSERT_EQUAL_INT(6, knob_mode_from_name("motor"));
    TEST_ASSERT_EQUAL_STRING("unknown", knob_mode_name(7));
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
    char mb[16];
    knob_setting_format(KNOB_SET_MOTOR_IDLE, 0, mb, sizeof(mb));
    TEST_ASSERT_EQUAL_STRING("NEVER", mb);
    knob_setting_format(KNOB_SET_MOTOR_STEPS, 2, mb, sizeof(mb));
    TEST_ASSERT_EQUAL_STRING("QUICK", mb);
    TEST_ASSERT_EQUAL_INT(60, knob_setting_step(KNOB_SET_MOTOR_AHEAD, 50, 1));
    TEST_ASSERT_EQUAL_INT(100, knob_setting_step(KNOB_SET_MOTOR_ASTERN, 100, 1));
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

/* ---- Music mode ---- */

static void test_music(void)
{
    TEST_ASSERT_EQUAL_INT(42, knob_vol_step(40, 1));
    TEST_ASSERT_EQUAL_INT(34, knob_vol_step(40, -3));
    TEST_ASSERT_EQUAL_INT(100, knob_vol_step(99, 1));   /* clamped */
    TEST_ASSERT_EQUAL_INT(0, knob_vol_step(1, -1));
    TEST_ASSERT_EQUAL_INT(42, knob_vol_pct(0.42));
    TEST_ASSERT_EQUAL_INT(43, knob_vol_pct(0.425));
    TEST_ASSERT_EQUAL_INT(100, knob_vol_pct(1.3));
    TEST_ASSERT_EQUAL_INT(0, knob_vol_pct(-0.1));
    char id[24], key[16];
    TEST_ASSERT_TRUE(knob_jukebox_zone_path("entertainment.jukebox.zones.b8-27-eb-01.volume", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("b8-27-eb-01", id);
    TEST_ASSERT_EQUAL_STRING("volume", key);
    TEST_ASSERT_TRUE(knob_jukebox_zone_path("entertainment.jukebox.zones.z1.connected", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("connected", key);
    TEST_ASSERT_FALSE(knob_jukebox_zone_path("entertainment.jukebox.playback.volume", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_FALSE(knob_jukebox_zone_path("entertainment.jukebox.zones.z1", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_FALSE(knob_jukebox_zone_path("entertainment.jukebox.zones..volume", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_FALSE(knob_jukebox_zone_path("entertainment.jukebox.zones.z1.a.b", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_FALSE(knob_jukebox_zone_path("entertainment.jukebox.zones.a-very-long-zone-identifier-x.volume", id, sizeof(id), key, sizeof(key)));
    TEST_ASSERT_FALSE(knob_jukebox_zone_path(NULL, id, sizeof(id), key, sizeof(key)));
}

/* ---- Autopilot states (#7) ---- */

static void test_ap_states(void)
{
    TEST_ASSERT_EQUAL_INT(KNOB_AP_WIND, knob_ap_state_from("wind"));
    TEST_ASSERT_EQUAL_INT(KNOB_AP_STANDBY, knob_ap_state_from("standby"));
    TEST_ASSERT_EQUAL_INT(KNOB_AP_UNKNOWN, knob_ap_state_from("off-line"));
    TEST_ASSERT_EQUAL_INT(KNOB_AP_UNKNOWN, knob_ap_state_from(NULL));
    TEST_ASSERT_EQUAL_STRING("auto", knob_ap_state_name(KNOB_AP_AUTO));
    TEST_ASSERT_NULL(knob_ap_state_name(KNOB_AP_UNKNOWN));
}

static void test_pilot_lines(void)
{
    knob_pilot_line_t l[7];
    int n = knob_pilot_lines(KNOB_AP_AUTO, l, 7);       /* compass: standby, wind, route, both tacks, cancel */
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_STANDBY, l[0]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_WIND, l[1]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_ROUTE, l[2]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_TACK_PORT, l[3]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_CANCEL, l[5]);
    n = knob_pilot_lines(KNOB_AP_ROUTE, l, 7);          /* route: no tacks */
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_COMPASS, l[1]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_WIND, l[2]);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_CANCEL, l[3]);
    n = knob_pilot_lines(KNOB_AP_WIND, l, 7);
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_INT(KNOB_PL_COMPASS, l[1]);
    n = knob_pilot_lines(KNOB_AP_WIND, l, 3);           /* capped */
    TEST_ASSERT_EQUAL_INT(3, n);
}

static void test_wind_and_xte(void)
{
    char side;
    TEST_ASSERT_EQUAL_INT(118, knob_wind_abs(-118.0f, &side));
    TEST_ASSERT_EQUAL_INT('P', side);
    TEST_ASSERT_EQUAL_INT(45, knob_wind_abs(45.0f, &side));
    TEST_ASSERT_EQUAL_INT('S', side);
    TEST_ASSERT_EQUAL_INT(178, knob_wind_abs(182.0f, &side));   /* wraps: 182 is 178 to port */
    TEST_ASSERT_EQUAL_INT('P', side);
    TEST_ASSERT_EQUAL_INT(180, knob_wind_abs(-180.0f, &side));
    TEST_ASSERT_EQUAL_INT('S', side);
    TEST_ASSERT_EQUAL_INT(0, knob_wind_abs(-0.2f, &side));
    TEST_ASSERT_EQUAL_INT('S', side);
    TEST_ASSERT_EQUAL_INT(-1, knob_xte_steer(150.0f, 10.0f)); /* right of the track: steer left */
    TEST_ASSERT_EQUAL_INT(1, knob_xte_steer(-150.0f, 10.0f));
    TEST_ASSERT_EQUAL_INT(0, knob_xte_steer(8.0f, 10.0f));
    /* The tack confirm is the confirm turn towards the tack side: port turns left, so the turn is passed negated. */
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_DONE, knob_confirm_step(0, 100, 5000, KNOB_CONFIRM_EV_TURN, -(-1)));
    TEST_ASSERT_EQUAL_INT(KNOB_CONFIRM_WAITING, knob_confirm_step(0, 100, 5000, KNOB_CONFIRM_EV_TURN, -(1)));
}

static void test_adjust_steps(void)
{
    int s[32];
    int n = knob_adjust_steps(100, 123, s, 32);
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_INT(10, s[0]);
    TEST_ASSERT_EQUAL_INT(10, s[1]);
    TEST_ASSERT_EQUAL_INT(1, s[2]);
    n = knob_adjust_steps(350, 5, s, 32);          /* across north, the short way */
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_INT(10, s[0]);
    TEST_ASSERT_EQUAL_INT(1, s[5]);
    n = knob_adjust_steps(5, 350, s, 32);
    TEST_ASSERT_EQUAL_INT(-10, s[0]);
    TEST_ASSERT_EQUAL_INT(-1, s[5]);
    TEST_ASSERT_EQUAL_INT(0, knob_adjust_steps(90, 90, s, 32));
    TEST_ASSERT_EQUAL_INT(3, knob_adjust_steps(0, 90, s, 3)); /* capped */
}

static void test_led_fill(void)
{
    uint16_t o[4];
    knob_led_fill(0.0f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(0, o[0]);
    TEST_ASSERT_EQUAL_UINT16(0, o[3]);
    knob_led_fill(0.125f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(500, o[0]);
    TEST_ASSERT_EQUAL_UINT16(0, o[1]);
    knob_led_fill(0.25f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(1000, o[0]);
    TEST_ASSERT_EQUAL_UINT16(0, o[1]);
    knob_led_fill(0.5f, 4, o);                     /* the left two full, the right two off */
    TEST_ASSERT_EQUAL_UINT16(1000, o[1]);
    TEST_ASSERT_EQUAL_UINT16(0, o[2]);
    knob_led_fill(0.625f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(1000, o[1]);
    TEST_ASSERT_EQUAL_UINT16(500, o[2]);
    TEST_ASSERT_EQUAL_UINT16(0, o[3]);
    knob_led_fill(1.0f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(1000, o[3]);
    knob_led_fill(1.5f, 4, o);                     /* clamped */
    TEST_ASSERT_EQUAL_UINT16(1000, o[3]);
    knob_led_fill(-0.3f, 4, o);
    TEST_ASSERT_EQUAL_UINT16(0, o[0]);
    knob_led_fill(0.75f, 2, o);                    /* the motor's half-range sides (#8) */
    TEST_ASSERT_EQUAL_UINT16(1000, o[0]);
    TEST_ASSERT_EQUAL_UINT16(500, o[1]);
}

/* ---- Motor mode (#8) ---- */

static knob_motor_in_t motor_in(uint32_t now)
{
    knob_motor_in_t in = { .now_ms = now, .allow_control = true, .link_ok = true, .gear = KNOB_GEAR_NEUTRAL,
                           .gear_ms = now, .max_ahead_pct = 50, .max_astern_pct = 50, .idle_s = 60,
                           .curve = KNOB_MOTOR_CURVE_NORMAL };
    return in;
}

/* Arms at t (long press, confirm click) and returns the time after the settle window. */
static uint32_t motor_arm(knob_motor_t *m, uint32_t t)
{
    knob_motor_in_t in = motor_in(t);
    TEST_ASSERT_TRUE(knob_motor_long_press(m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_CONFIRM, m->state);
    in.now_ms = t + 1000;
    knob_motor_turn(m, &in, 1);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m->state);
    TEST_ASSERT_EQUAL_INT(0, m->set_pct);
    return t + 1000 + KNOB_MOTOR_SETTLE_MS + 200;
}

/* n slow clicks (1 % each with every curve) in direction dir, 200 ms apart, gear following the throttle. */
static uint32_t motor_clicks(knob_motor_t *m, uint32_t t, int dir, int n)
{
    for (int i = 0; i < n; i++) {
        t += 200;
        knob_motor_in_t in = motor_in(t);
        knob_motor_turn(m, &in, dir);
    }
    return t;
}

static void test_motor_steps(void)
{
    TEST_ASSERT_EQUAL_INT(1, knob_motor_step(KNOB_MOTOR_CURVE_NORMAL, 151));
    TEST_ASSERT_EQUAL_INT(2, knob_motor_step(KNOB_MOTOR_CURVE_NORMAL, 150));
    TEST_ASSERT_EQUAL_INT(2, knob_motor_step(KNOB_MOTOR_CURVE_NORMAL, 50));
    TEST_ASSERT_EQUAL_INT(5, knob_motor_step(KNOB_MOTOR_CURVE_NORMAL, 49));
    TEST_ASSERT_EQUAL_INT(2, knob_motor_step(KNOB_MOTOR_CURVE_GENTLE, 10));
    TEST_ASSERT_EQUAL_INT(10, knob_motor_step(KNOB_MOTOR_CURVE_QUICK, 10));
    TEST_ASSERT_EQUAL_INT(3, knob_motor_step(KNOB_MOTOR_CURVE_QUICK, 100));
    TEST_ASSERT_EQUAL_INT(5, knob_motor_step(99, 10));   /* unknown preset = normal */
}

static void test_motor_arming(void)
{
    knob_motor_t m;
    knob_motor_init(&m);
    int8_t v;
    TEST_ASSERT_FALSE(knob_motor_take_send(&m, 5000, &v));   /* nothing after a reboot */

    knob_motor_in_t in = motor_in(1000);
    in.allow_control = false;
    TEST_ASSERT_FALSE(knob_motor_long_press(&m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_CONTROL_OFF, m.why);
    in = motor_in(1000);
    in.gear = KNOB_GEAR_FORWARD;
    TEST_ASSERT_FALSE(knob_motor_long_press(&m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_NOT_NEUTRAL, m.why);
    in = motor_in(1000);
    in.gear_ms = 0;                                          /* no gear published */
    TEST_ASSERT_FALSE(knob_motor_long_press(&m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_NO_DATA, m.why);
    in = motor_in(10000);
    in.gear_ms = 6000;                                       /* 4 s old */
    TEST_ASSERT_FALSE(knob_motor_long_press(&m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_NO_DATA, m.why);
    in = motor_in(1000);
    in.gear = KNOB_GEAR_FAULT;
    TEST_ASSERT_FALSE(knob_motor_long_press(&m, &in));
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_FAULT, m.why);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);

    /* the confirm: a left click cancels, the timeout cancels */
    in = motor_in(2000);
    TEST_ASSERT_TRUE(knob_motor_long_press(&m, &in));
    knob_motor_turn(&m, &in, -1);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
    TEST_ASSERT_TRUE(knob_motor_long_press(&m, &in));
    in.now_ms = 2000 + KNOB_MOTOR_CONFIRM_MS;
    in.gear_ms = in.now_ms;
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_CANCELLED, m.why);

    /* armed: 0 goes out once, the confirm turn's follow-through does nothing */
    uint32_t t = 20000;
    in = motor_in(t);
    knob_motor_long_press(&m, &in);
    in.now_ms = t + 500;
    knob_motor_turn(&m, &in, 1);
    TEST_ASSERT_TRUE(knob_motor_take_send(&m, t + 500, &v));
    TEST_ASSERT_EQUAL_INT(0, v);
    for (int i = 1; i <= 5; i++) { in.now_ms = t + 500 + 20 * i; knob_motor_turn(&m, &in, 1); }
    TEST_ASSERT_EQUAL_INT(0, m.set_pct);
    TEST_ASSERT_FALSE(knob_motor_take_send(&m, t + 700, &v));
}

static void test_motor_neutral_stop(void)
{
    knob_motor_t m;
    knob_motor_init(&m);
    uint32_t t = motor_arm(&m, 1000);
    t = motor_clicks(&m, t, 1, 10);
    TEST_ASSERT_EQUAL_INT(10, m.set_pct);
    /* a fast spin to the left stops at 0 and stays there */
    for (int i = 0; i < 20; i++) { t += 20; knob_motor_in_t in = motor_in(t); knob_motor_turn(&m, &in, -1); }
    TEST_ASSERT_EQUAL_INT(0, m.set_pct);
    /* still spinning slowly but less than the pause apart: still held */
    t = motor_clicks(&m, t, -1, 3);
    TEST_ASSERT_EQUAL_INT(0, m.set_pct);
    /* after the pause, astern */
    t += KNOB_MOTOR_PAUSE_MS;
    knob_motor_in_t in = motor_in(t);
    knob_motor_turn(&m, &in, -1);
    TEST_ASSERT_EQUAL_INT(-1, m.set_pct);
    /* back to ahead through 0 also stops */
    t = motor_clicks(&m, t, 1, 5);
    TEST_ASSERT_EQUAL_INT(0, m.set_pct);
    /* back to the side it came from needs no pause */
    t = motor_clicks(&m, t, -1, 1);
    TEST_ASSERT_EQUAL_INT(-1, m.set_pct);
}

static void test_motor_presses(void)
{
    knob_motor_t m;
    knob_motor_init(&m);
    uint32_t t = motor_arm(&m, 1000);
    t = motor_clicks(&m, t, 1, 8);
    knob_motor_in_t in = motor_in(t + 100);
    in.gear = KNOB_GEAR_FORWARD;
    TEST_ASSERT_FALSE(knob_motor_double_press(&m, &in));     /* no warning: the UI opens the ring */
    TEST_ASSERT_TRUE(knob_motor_long_press(&m, &in));        /* out of neutral: 0, stays armed */
    TEST_ASSERT_EQUAL_INT(0, m.set_pct);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m.state);
    int8_t v;
    TEST_ASSERT_TRUE(knob_motor_take_send(&m, t + 100, &v));
    TEST_ASSERT_EQUAL_INT(0, v);
    /* after a long press either direction goes at once */
    t = motor_clicks(&m, t + 100, -1, 1);
    TEST_ASSERT_EQUAL_INT(-1, m.set_pct);
    in = motor_in(t + 100);
    knob_motor_long_press(&m, &in);
    TEST_ASSERT_TRUE(knob_motor_long_press(&m, &in));        /* in neutral: disarm */
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_USER, m.why);
}

static void test_motor_limits(void)
{
    knob_motor_t m;
    knob_motor_init(&m);
    uint32_t t = motor_arm(&m, 1000);
    t = motor_clicks(&m, t, 1, 60);
    TEST_ASSERT_EQUAL_INT(50, m.set_pct);                    /* held at the limit */
    TEST_ASSERT_TRUE(m.limit_warning);
    t = motor_clicks(&m, t, -1, 5);                          /* back below: the warning stays */
    TEST_ASSERT_TRUE(m.limit_warning);
    knob_motor_in_t in = motor_in(t);
    TEST_ASSERT_TRUE(knob_motor_double_press(&m, &in));
    TEST_ASSERT_FALSE(m.limit_warning);
    TEST_ASSERT_EQUAL_INT(45, m.set_pct);                    /* the double press raises nothing */
    t = motor_clicks(&m, t, 1, 60);
    TEST_ASSERT_EQUAL_INT(100, m.set_pct);
    /* astern is lifted too in this arm cycle */
    in = motor_in(t);
    knob_motor_long_press(&m, &in);
    t += KNOB_MOTOR_PAUSE_MS;
    t = motor_clicks(&m, t, -1, 60);
    TEST_ASSERT_EQUAL_INT(-60, m.set_pct);
    /* after a disarm the limits are back */
    in = motor_in(t);
    knob_motor_long_press(&m, &in);
    knob_motor_long_press(&m, &in);
    t = motor_arm(&m, t + 100);
    t = motor_clicks(&m, t, -1, 60);
    TEST_ASSERT_EQUAL_INT(-50, m.set_pct);
}

static void test_motor_disarm_reasons(void)
{
    knob_motor_t m;
    int8_t v;
    knob_motor_why_t whys[] = { KNOB_MOTOR_WHY_CONTROL_OFF, KNOB_MOTOR_WHY_NO_LINK, KNOB_MOTOR_WHY_NO_DATA, KNOB_MOTOR_WHY_FAULT };
    for (int k = 0; k < 4; k++) {
        knob_motor_init(&m);
        uint32_t t = motor_arm(&m, 1000);
        t = motor_clicks(&m, t, 1, 5);
        knob_motor_take_send(&m, t, &v);
        knob_motor_in_t in = motor_in(t + 100);
        in.gear = KNOB_GEAR_FORWARD;
        if (k == 0) in.allow_control = false;
        if (k == 1) in.link_ok = false;
        if (k == 2) in.gear_ms = t + 100 - 3500;
        if (k == 3) in.gear = KNOB_GEAR_FAULT;
        knob_motor_tick(&m, &in);
        TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
        TEST_ASSERT_EQUAL_INT(whys[k], m.why);
        TEST_ASSERT_TRUE(knob_motor_take_send(&m, t + 100, &v));   /* 0 goes out once */
        TEST_ASSERT_EQUAL_INT(0, v);
        TEST_ASSERT_FALSE(knob_motor_take_send(&m, t + 5000, &v)); /* and then nothing, no keepalive */
    }

    /* a gear that does not follow the throttle for more than 3 s */
    knob_motor_init(&m);
    uint32_t t = motor_arm(&m, 1000);
    t = motor_clicks(&m, t, 1, 5);
    knob_motor_in_t in = motor_in(t);                        /* gear stays Neutral */
    knob_motor_tick(&m, &in);
    in = motor_in(t + 3000);
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m.state);
    in = motor_in(t + 3100);
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_FAULT, m.why);
}

static void test_motor_idle_and_keepalive(void)
{
    knob_motor_t m;
    int8_t v;
    knob_motor_init(&m);
    uint32_t t = motor_arm(&m, 1000);
    knob_motor_take_send(&m, t, &v);
    t = motor_clicks(&m, t, 1, 3);
    TEST_ASSERT_TRUE(knob_motor_take_send(&m, t, &v));
    TEST_ASSERT_EQUAL_INT(3, v);
    TEST_ASSERT_FALSE(knob_motor_take_send(&m, t + 900, &v));
    TEST_ASSERT_TRUE(knob_motor_take_send(&m, t + 1000, &v));    /* keepalive */
    TEST_ASSERT_EQUAL_INT(3, v);
    /* out of neutral it never times out */
    knob_motor_in_t in = motor_in(t + 120000);
    in.gear = KNOB_GEAR_FORWARD;
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m.state);
    /* in neutral it does, and no keepalive is due there */
    in.now_ms = t + 120000;
    knob_motor_long_press(&m, &in);
    knob_motor_take_send(&m, t + 120000, &v);
    TEST_ASSERT_FALSE(knob_motor_take_send(&m, t + 125000, &v));
    in = motor_in(t + 120000 + 59000);
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m.state);
    in = motor_in(t + 120000 + 60000);
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_DISARMED, m.state);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_WHY_IDLE, m.why);
    /* 0 = never */
    knob_motor_init(&m);
    t = motor_arm(&m, 1000);
    in = motor_in(t + 3600000);
    in.idle_s = 0;
    knob_motor_tick(&m, &in);
    TEST_ASSERT_EQUAL_INT(KNOB_MOTOR_ARMED, m.state);
}

static void test_gear_from_str(void)
{
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_FORWARD, knob_gear_from_str("\"Forward\""));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_NEUTRAL, knob_gear_from_str("neutral"));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_REVERSE, knob_gear_from_str("\"Reverse\""));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_FAULT, knob_gear_from_str("\"Fault\""));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_UNKNOWN, knob_gear_from_str("\"Forwards\""));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_UNKNOWN, knob_gear_from_str("null"));
    TEST_ASSERT_EQUAL_INT(KNOB_GEAR_UNKNOWN, knob_gear_from_str(NULL));
}

/* ---- Automatic demo player (#10) ---- */

/* A fake knob: it classifies button edges like marine_ui does (long press after 750 ms held, a single press 300 ms after
 * the release, two releases close together a double press) and runs a small mode ring and settings list. */
typedef struct {
    uint32_t now;
    bool down; uint32_t down_ms, last_up_ms; int clicks; bool long_fired;
    int singles, doubles, longs, turns_cw, turns_ccw, touches;
    int mode, ring_open, ring_focus, set_index;
    int fx_turn, fx_press, fx_double, fx_long, fx_count_last, fx_clear;
    const char *caption;
    bool cond_ok; int events[8]; int n_events;
} fake_t;

static const int ring_order[] = { 0, 1, 2, 5, 3, 4 };   /* autopilot, lights, anchor, music, USB, settings */

static int ring_next(int m)
{
    for (int i = 0; i < 6; i++) if (ring_order[i] == m) return ring_order[(i + 1) % 6];
    return 0;
}

static void f_semantic(fake_t *f, int kind) /* 1 single, 2 double, 3 long */
{
    if (kind == 1) { f->singles++; if (f->ring_open) { f->mode = f->ring_focus; f->ring_open = 0; } }
    if (kind == 2) { f->doubles++; f->ring_open = !f->ring_open; f->ring_focus = f->mode; }
    if (kind == 3) f->longs++;
}

static void f_poll(fake_t *f)
{
    if (f->down && !f->long_fired && f->now - f->down_ms >= 750) { f->long_fired = true; f->clicks = 0; f_semantic(f, 3); }
    if (!f->down && f->clicks == 1 && f->now - f->last_up_ms >= 300) { f->clicks = 0; f_semantic(f, 1); }
}

static void f_turn(void *c, int dir)
{
    fake_t *f = c;
    if (dir > 0) f->turns_cw++; else f->turns_ccw++;
    if (f->ring_open) f->ring_focus = ring_next(f->ring_focus);
    else if (f->mode == KNOB_MODE_SETTINGS) f->set_index += dir;
}
static void f_button(void *c, bool down)
{
    fake_t *f = c;
    if (down) { f->down = true; f->down_ms = f->now; f->long_fired = false; return; }
    f->down = false;
    if (f->long_fired) return;
    f->clicks++;
    f->last_up_ms = f->now;
    if (f->clicks >= 2) { f->clicks = 0; f_semantic(f, 2); }
}
static void f_touch(void *c, int16_t x, int16_t y, bool down) { (void)x; (void)y; if (down) ((fake_t *)c)->touches++; }
static void f_show(void *c, knob_ad_fx_t fx, int a, int b)
{
    fake_t *f = c;
    (void)b;
    if (fx == KNOB_AD_FX_TURN) f->fx_turn++;
    if (fx == KNOB_AD_FX_PRESS) f->fx_press++;
    if (fx == KNOB_AD_FX_DOUBLE) f->fx_double++;
    if (fx == KNOB_AD_FX_LONG) f->fx_long++;
    if (fx == KNOB_AD_FX_COUNT) f->fx_count_last = a;
    if (fx == KNOB_AD_FX_CLEAR) f->fx_clear++;
}
static void f_caption(void *c, const char *t) { ((fake_t *)c)->caption = t; }
static bool f_cond(void *c, int id) { (void)id; return ((fake_t *)c)->cond_ok; }
static void f_event(void *c, int id) { fake_t *f = c; if (f->n_events < 8) f->events[f->n_events++] = id; if (id == KNOB_AD_EV_RESET) f->mode = 0; }
static int f_query(void *c, knob_ad_query_t q)
{
    fake_t *f = c;
    switch (q) {
    case KNOB_AD_Q_MODE: return f->mode;
    case KNOB_AD_Q_RING_OPEN: return f->ring_open;
    case KNOB_AD_Q_RING_FOCUS: return f->ring_focus;
    case KNOB_AD_Q_SET_INDEX: return f->mode == KNOB_MODE_SETTINGS ? f->set_index : -1;
    }
    return -1;
}

static knob_ad_ops_t fake_ops(fake_t *f)
{
    knob_ad_ops_t o = { f_turn, f_button, f_touch, f_show, f_caption, f_cond, f_event, f_query, f };
    return o;
}

/* Runs the player and the fake UI in 5 ms steps for ms milliseconds. */
static void run_ms(knob_ad_t *p, fake_t *f, const knob_ad_ops_t *o, uint32_t ms)
{
    for (uint32_t end = f->now + ms; f->now < end; f->now += 5) {
        knob_ad_tick(p, f->now, o);
        f_poll(f);
    }
}

static void test_autodemo_gestures(void)
{
    static const knob_ad_step_t s[] = {
        { KNOB_AD_PRESS, 0, 0, 0, NULL }, { KNOB_AD_LONG, 0, 0, 0, NULL }, { KNOB_AD_DOUBLE, 0, 0, 0, NULL },
        { KNOB_AD_TURN, -3, 0, 0, NULL }, { KNOB_AD_TOUCH, 200, 120, 0, NULL }, { KNOB_AD_WAIT, 0, 0, 60000, NULL },
        { KNOB_AD_END, 0, 0, 0, NULL },
    };
    fake_t f = { .now = 1000 };
    knob_ad_ops_t o = fake_ops(&f);
    knob_ad_t p;
    knob_ad_init(&p, s);
    knob_ad_start(&p, f.now);
    run_ms(&p, &f, &o, 8000);
    TEST_ASSERT_EQUAL_INT(1, f.singles);   /* the press resolved as a single press, not as half of a double */
    TEST_ASSERT_EQUAL_INT(1, f.longs);
    TEST_ASSERT_EQUAL_INT(1, f.doubles);
    TEST_ASSERT_EQUAL_INT(3, f.turns_ccw);
    TEST_ASSERT_EQUAL_INT(1, f.touches);
    TEST_ASSERT_EQUAL_INT(3, f.fx_turn);   /* every click shown */
    TEST_ASSERT_EQUAL_INT(1, f.fx_press);
    TEST_ASSERT_EQUAL_INT(1, f.fx_long);
    TEST_ASSERT_EQUAL_INT(1, f.fx_double);
    TEST_ASSERT_FALSE(f.down);
}

static void test_autodemo_until_and_countdown(void)
{
    static const knob_ad_step_t s[] = {
        { KNOB_AD_COUNTDOWN, 3, 0, 0, NULL }, { KNOB_AD_UNTIL, 1, 0, 2000, NULL }, { KNOB_AD_CAPTION, 0, 0, 0, "HI" },
        { KNOB_AD_WAIT, 0, 0, 60000, NULL }, { KNOB_AD_END, 0, 0, 0, NULL },
    };
    fake_t f = { .now = 0 };
    knob_ad_ops_t o = fake_ops(&f);
    knob_ad_t p;
    knob_ad_init(&p, s);
    knob_ad_start(&p, f.now);
    run_ms(&p, &f, &o, 1500);
    TEST_ASSERT_EQUAL_INT(2, f.fx_count_last);      /* 3 at 0 s, 2 at 1 s */
    run_ms(&p, &f, &o, 2000);
    TEST_ASSERT_EQUAL_INT(0, f.fx_count_last);      /* hidden after 1 */
    TEST_ASSERT_NULL(f.caption);                    /* the condition never holds: still waiting for the timeout */
    run_ms(&p, &f, &o, 1600);
    TEST_ASSERT_EQUAL_STRING("HI", f.caption);      /* moved on after the 2 s timeout */
}

static void test_autodemo_ring_and_settings(void)
{
    static const knob_ad_step_t s[] = {
        { KNOB_AD_RING_TO, KNOB_MODE_MUSIC, 0, 0, NULL }, { KNOB_AD_RING_TO, KNOB_MODE_MUSIC, 0, 0, NULL },
        { KNOB_AD_RING_TO, KNOB_MODE_SETTINGS, 0, 0, NULL }, { KNOB_AD_SET_TO, KNOB_SET_DEMO, 0, 0, NULL },
        { KNOB_AD_WAIT, 0, 0, 60000, NULL }, { KNOB_AD_END, 0, 0, 0, NULL },
    };
    fake_t f = { .now = 0 };
    knob_ad_ops_t o = fake_ops(&f);
    knob_ad_t p;
    knob_ad_init(&p, s);
    knob_ad_start(&p, f.now);
    run_ms(&p, &f, &o, 4000);
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_MUSIC, f.mode);
    run_ms(&p, &f, &o, 14000);
    TEST_ASSERT_EQUAL_INT(2, f.doubles);            /* music and settings: the second RING_TO to music did nothing */
    TEST_ASSERT_EQUAL_INT(KNOB_MODE_SETTINGS, f.mode);
    TEST_ASSERT_EQUAL_INT(0, f.ring_open);
    TEST_ASSERT_EQUAL_INT(KNOB_SET_DEMO, f.set_index);
}

static void test_autodemo_pause_and_restart(void)
{
    static const knob_ad_step_t s[] = {
        { KNOB_AD_EVENT, 7, 0, 0, NULL }, { KNOB_AD_LONG, 0, 0, 0, NULL }, { KNOB_AD_WAIT, 0, 0, 60000, NULL },
        { KNOB_AD_END, 0, 0, 0, NULL },
    };
    fake_t f = { .now = 0 };
    knob_ad_ops_t o = fake_ops(&f);
    knob_ad_t p;
    knob_ad_init(&p, s);
    knob_ad_start(&p, f.now);
    run_ms(&p, &f, &o, 300);                        /* in the middle of the long press */
    TEST_ASSERT_TRUE(f.down);
    knob_ad_user_input(&p, f.now, &o);
    TEST_ASSERT_FALSE(f.down);                      /* the held button is let go */
    TEST_ASSERT_EQUAL_INT(KNOB_AD_PAUSED, p.state);
    TEST_ASSERT_EQUAL_INT(1, f.fx_clear);
    run_ms(&p, &f, &o, KNOB_AD_IDLE_RESTART_MS - 1000);
    TEST_ASSERT_EQUAL_INT(1, f.n_events);           /* still paused */
    run_ms(&p, &f, &o, 2000);
    TEST_ASSERT_EQUAL_INT(KNOB_AD_RUNNING, p.state);
    TEST_ASSERT_EQUAL_INT(2, f.n_events);           /* started again from the top */
}

/* The real script runs to its end and starts again, with every condition holding. */
static void test_autodemo_script_loops(void)
{
    fake_t f = { .now = 0, .cond_ok = true };
    knob_ad_ops_t o = fake_ops(&f);
    knob_ad_t p;
    knob_ad_init(&p, knob_ad_script);
    knob_ad_start(&p, f.now);
    run_ms(&p, &f, &o, 240000);
    TEST_ASSERT_TRUE(p.loops >= 1);
    TEST_ASSERT_EQUAL_INT(KNOB_AD_EV_RESET, f.events[0]);
    TEST_ASSERT_TRUE(f.longs >= 6);                 /* engage, light off, drop, two profile changes */
    TEST_ASSERT_TRUE(f.doubles >= 6);               /* six trips through the ring */
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
    RUN_TEST(test_music);
    RUN_TEST(test_ap_states);
    RUN_TEST(test_pilot_lines);
    RUN_TEST(test_wind_and_xte);
    RUN_TEST(test_adjust_steps);
    RUN_TEST(test_led_fill);
    RUN_TEST(test_motor_steps);
    RUN_TEST(test_motor_arming);
    RUN_TEST(test_motor_neutral_stop);
    RUN_TEST(test_motor_presses);
    RUN_TEST(test_motor_limits);
    RUN_TEST(test_motor_disarm_reasons);
    RUN_TEST(test_motor_idle_and_keepalive);
    RUN_TEST(test_gear_from_str);
    RUN_TEST(test_autodemo_gestures);
    RUN_TEST(test_autodemo_until_and_countdown);
    RUN_TEST(test_autodemo_ring_and_settings);
    RUN_TEST(test_autodemo_pause_and_restart);
    RUN_TEST(test_autodemo_script_loops);
    return UNITY_END();
}
