/* PC renderer for screenshots: the real marine_ui and knob_logic with LVGL, no hardware. feed, action and snapshot follow
 * knob_model.c's demo_feed, demo_action and build_snapshot for the automatic demo (its start state from ad_event). */
#include "marine_ui.h"
#include "knob_logic.h"
#include "knob_demo.h"
#include "knob_motor.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
void *lv_malloc_core(size_t n) { return malloc(n); }
void *lv_realloc_core(void *p, size_t n) { return realloc(p, n); }
void lv_free_core(void *p) { free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t *m) { memset(m, 0, sizeof *m); }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

static uint32_t now = 60000;
static uint32_t tick(void) { return now; }
static uint16_t fb[240 * 240];
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    uint16_t *px = (uint16_t *)p;
    int w = lv_area_get_width(a);
    for (int y = a->y1; y <= a->y2; y++, px += w) memcpy(&fb[y * 240 + a->x1], px, w * 2);
    lv_display_flush_ready(d);
}

static knob_demo_t dm;
static knob_motor_t mo;
static marine_ui_snapshot_t s;
static int vol = 42, play = MARINE_PLAY_PLAYING, hid = HID_PROFILE_NAV_KEYS, apst = 0, radius = 35, pos = 53000;
static bool ack, night, gps_lost;
static uint16_t seq;

static uint8_t gear(void) { return mo.set_pct > 0 ? KNOB_GEAR_FORWARD : mo.set_pct < 0 ? KNOB_GEAR_REVERSE : KNOB_GEAR_NEUTRAL; }
static knob_motor_in_t motor_in(void)
{
    knob_motor_in_t i = { 0 };
    i.now_ms = now; i.allow_control = i.link_ok = true; i.gear = gear(); i.gear_ms = now;
    i.max_ahead_pct = i.max_astern_pct = 50; i.idle_s = 60; i.curve = 1;
    return i;
}

static void act(const marine_ui_action_t *a, void *c)
{
    knob_motor_in_t in = motor_in();
    bool rep = false;
    switch (a->action) {
    case MARINE_ACT_AP_ENGAGE: knob_demo_engage(&dm, true); apst = 1; break;
    case MARINE_ACT_AP_SET_STATE: apst = a->value; break;
    case MARINE_ACT_AP_STANDBY: knob_demo_engage(&dm, false); break;
    case MARINE_ACT_AP_SET_CTS: knob_demo_set_target(&dm, a->value); break;
    case MARINE_ACT_ANCHOR_SET_RADIUS: knob_demo_set_radius(&dm, a->value); rep = dm.anchor_down; radius = a->value; break;
    case MARINE_ACT_ANCHOR_DROP:
    case MARINE_ACT_ANCHOR_MOVE_DROP: knob_demo_drop(&dm, 0); rep = true; break;
    case MARINE_ACT_ANCHOR_RAISE: knob_demo_raise(&dm); rep = true; break;
    case MARINE_ACT_DIMMER_TOGGLE_POWER: knob_demo_set_light(&dm, a->value, !dm.light[a->value]); break;
    case MARINE_ACT_DIMMER_SET_LEVEL: knob_demo_set_level(&dm, a->meta, a->value); break;
    case MARINE_ACT_VOL_SET: if (!a->meta) vol = a->value; break;
    case MARINE_ACT_HID_NEXT_PROFILE: hid = (hid + 1) % HID_PROFILE_COUNT; break;
    case MARINE_ACT_MOTOR_TURN: knob_motor_turn(&mo, &in, a->value > 0 ? 1 : -1); break;
    case MARINE_ACT_MOTOR_LONG: knob_motor_long_press(&mo, &in); break;
    case MARINE_ACT_ALARM_ACK: ack = true; break;
    default: break;
    }
    if (rep) { s.action_result = MARINE_RESULT_OK; s.action_result_seq = ++seq; }
}

static void apply(void)
{
    knob_motor_in_t in = motor_in();
    int8_t pc;
    knob_motor_tick(&mo, &in);
    knob_motor_take_send(&mo, now, &pc);
    knob_demo_step(&dm, now);
    s.now_ms = now;
    s.fx_glass_enabled = s.fx_glow_enabled = s.fx_ring_pulse_enabled = s.alarm_ack_return_with_badge = true;
    s.system_screen = MARINE_SCREEN_OPERATIONAL;
    s.night_palette = night;
    s.signalk_connected = true;
    s.last_data_ms = now;
    s.demo = true;
    s.heading_ref = 'T'; s.heading_deg = dm.heading_deg; s.heading_updated_ms = now; s.heading_timeout_ms = 5000;
    if (!dm.engaged) apst = 0;
    s.ap_engaged = dm.engaged; s.ap_state = apst; s.cts_deg = dm.target_deg;
    s.ap_state_updated_ms = s.cts_updated_ms = now; s.ap_state_timeout_ms = s.cts_timeout_ms = 15000;
    s.wind_awa_deg = -42 + 4 * sin(now / 7000.0); s.wind_updated_ms = now; s.wind_timeout_ms = 5000;
    s.rudder_deg = dm.rudder_deg; s.depth_m = dm.depth_m; s.voltage_v = dm.voltage_v;
    s.rudder_updated_ms = s.depth_updated_ms = s.voltage_updated_ms = now;
    s.rudder_timeout_ms = 5000; s.depth_timeout_ms = 10000; s.voltage_timeout_ms = 30000;
    s.gps_3d_fix = true; s.gps_updated_ms = now;
    s.anchor_dropped = dm.anchor_down; s.anchor_radius_m = dm.anchor_down ? lroundf(dm.radius_m) : radius;
    s.anchor_dist_m = dm.anchor_dist_m; s.anchor_bearing_deg = dm.anchor_bearing_deg;
    s.anchor_updated_ms = now; s.anchor_timeout_ms = dm.anchor_down ? 10000 : 0;
    s.active_alarm = knob_demo_dragging(&dm) ? MARINE_ALARM_ANCHOR_DRAG : MARINE_ALARM_NONE;
    if (gps_lost) s.active_alarm = MARINE_ALARM_GPS_LOST; /* like the bench alarm g */
    if (!s.active_alarm) ack = false;
    s.alarm_acknowledged = s.active_alarm && ack;
    static const char *names[] = { "SALOON", "COCKPIT", "DECK" };
    s.dimmer_group_count = 3;
    for (int i = 0; i < 3; i++) {
        marine_dimmer_group_t *g = &s.dimmer_groups[i];
        strcpy(g->name, names[i]);
        g->onoff_only = i == 2;
        g->on = dm.light[i];
        g->level_pct = g->on ? (i == 2 ? 100 : dm.level[i]) : 0;
    }
    s.dimmer_updated_ms = now;
    s.music_mode = s.ap_mode = s.lights_mode = s.anchor_mode = s.usb_mode = s.motor_mode = true;
    s.vol_target_count = 1;
    marine_vol_target_t *t = s.vol_targets;
    strcpy(t->name, "MASTER"); t->pct = vol; t->connected = true; t->updated_ms = now;
    s.play_state = play;
    strcpy(s.track, "Robotti humppa"); strcpy(s.artist, "El\xC3\xA4kel\xC3\xA4iset"); s.track_pos_s = pos / 1000;
    s.motor_state = mo.state; s.motor_set_pct = mo.set_pct; s.motor_limit_warning = mo.limit_warning; s.motor_limit_lifted = mo.limit_lifted;
    s.motor_max_ahead_pct = s.motor_max_astern_pct = 50;
    s.motor_why = mo.why; s.motor_why_ms = mo.why_ms; s.motor_confirm_ms = mo.confirm_ms;
    s.motor_gear = gear(); s.motor_gear_ms = now;
    double a = fabs(mo.set_pct / 100.0);
    s.motor_rpm = a * 3000; s.motor_torque_pct = a * 80; s.motor_power_w = a * 4000; s.motor_reported_pct = mo.set_pct;
    s.motor_rpm_ms = s.motor_torque_ms = s.motor_reported_ms = now;
    s.motor_power_ms = 0; /* the default: no power path set */
    s.usb_host_enumerated = true;
    s.hid_profile = hid;
    int16_t *v = s.setting;
    v[KNOB_SET_BRIGHTNESS] = 100; v[KNOB_SET_DIM_AFTER] = 30; v[KNOB_SET_DIM_LEVEL] = 10; v[KNOB_SET_OFF_AFTER] = 10;
    v[KNOB_SET_LEDS] = 1; v[KNOB_SET_DEMO] = 3;
    v[KNOB_SET_MOTOR_STEPS] = 1; v[KNOB_SET_MOTOR_AHEAD] = v[KNOB_SET_MOTOR_ASTERN] = 50; v[KNOB_SET_MOTOR_IDLE] = 60;
    marine_ui_apply_snapshot(&s);
}

static void run(int ms)
{
    for (int i = 0; i < ms; i += 10) {
        now += 10;
        if (play == MARINE_PLAY_PLAYING) pos += 10;
        if (now % 50 == 0) marine_ui_tick(now);
        if (now % 100 == 0) apply();
        lv_timer_handler();
    }
}
static void turn(int n) { for (int i = 0; i < abs(n); i++) { marine_ui_feed_encoder_step(n > 0 ? 1 : -1); run(220); } }
static void edge(bool down, int ms) { marine_ui_feed_button_edge(down); run(ms); }
static void press(void) { edge(true, 90); edge(false, 420); }
static void dbl(void) { edge(true, 90); edge(false, 110); edge(true, 90); edge(false, 420); }
static void lng(void) { edge(true, 950); edge(false, 150); }
static void ringto(int m)
{
    if (!marine_ui_ring_is_open()) dbl();
    for (int i = 0; i < 9 && (int)marine_ui_ring_focus() != m; i++) turn(1);
    press();
    run(1200);
    if ((int)marine_ui_get_active_mode() != m) printf("MISSED mode %d\n", m);
}
static void shot(const char *n)
{
    char f[99];
    lv_refr_now(NULL);
    sprintf(f, "out/%s.ppm", n);
    FILE *o = fopen(f, "wb");
    fprintf(o, "P6 240 240 255\n");
    for (int i = 0; i < 240 * 240; i++) {
        int r = fb[i] >> 11, g = fb[i] >> 5 & 63, b = fb[i] & 31;
        fputc(r * 255 / 31, o); fputc(g * 255 / 63, o); fputc(b * 255 / 31, o);
    }
    fclose(o);
    printf("%s mode %d\n", n, marine_ui_get_active_mode());
}
static void both(const char *n) /* day and night of the same screen */
{
    char b[64];
    shot(n);
    night = true; run(400);
    sprintf(b, "%s-night", n);
    shot(b);
    night = false; run(400);
}

static void brightest(const char *n) /* a pulsing screen at the top of its pulse */
{
    long best = -1;
    for (int i = 0; i < 40; i++) {
        long sum = 0;
        run(50);
        lv_refr_now(NULL);
        for (int k = 0; k < 240 * 240; k++) sum += fb[k] >> 11;
        if (sum > best) { best = sum; shot(n); }
    }
}

int main(void)
{
    static uint8_t buf[240 * 240 * 2] __attribute__((aligned(8)));
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(240, 240);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
    marine_ui_set_power(0, 10, 0);
    marine_ui_init(act, NULL);
    knob_demo_init(&dm, now, 35);
    dm.swing_m = 17; knob_demo_set_swing(&dm, 17);
    knob_demo_set_level(&dm, 0, 80); knob_demo_set_level(&dm, 1, 40); knob_demo_set_light(&dm, 2, false);
    knob_motor_init(&mo);
    apply();
    marine_ui_set_mode(MARINE_MODE_AUTOPILOT);
    run(8000);
    shot("autopilot-standby");
    lng(); run(700); shot("autopilot-confirm");
    turn(1); run(2000); press(); run(600); turn(-2); run(700); press(); run(600); turn(-3); run(3500);
    both("autopilot");
    run(15000);
    dbl(); run(600); shot("mode-ring"); dbl(); run(800); night = true; run(500); dbl(); run(600); shot("mode-ring-night"); dbl(); run(800); night = false; run(500);
    ringto(MARINE_MODE_ANCHOR_WATCH);
    turn(-3); run(800); shot("anchor-radius"); press(); run(1500);
    lng(); run(1000); shot("anchor-dialog"); press(); run(25000); both("anchor");
    ringto(MARINE_MODE_DIMMER); turn(-4); run(1500); both("lights");
    ringto(MARINE_MODE_MUSIC); run(1000); both("music");
    ringto(MARINE_MODE_USB_HID); lng(); run(1300); lng(); run(1500); both("usb");
    ringto(MARINE_MODE_SETTINGS); run(800); shot("settings");
    for (int i = 0; i < 25 && marine_ui_settings_index() != KNOB_SET_MODES; i++) turn(1);
    run(500); press(); run(900); shot("settings-modes"); dbl(); run(800);
    ringto(MARINE_MODE_MOTOR);
    lng(); run(400); shot("motor-confirm"); turn(1); run(1500); turn(24); run(2000); both("motor");
    lng(); run(500); lng(); run(800);
    ringto(MARINE_MODE_ANCHOR_WATCH);
    knob_demo_set_swing(&dm, 26); run(20000); for (int i = 0; i < 600 && dm.anchor_dist_m < 24; i++) run(100); brightest("drag-alarm");
    press(); run(2000); shot("drag-alarm-acknowledged");
    knob_demo_set_radius(&dm, 60); run(1000);
    gps_lost = true; run(1000); brightest("gps-alarm");
    night = true; run(1000); brightest("gps-alarm-night");
    gps_lost = false; run(1000);
    knob_demo_set_radius(&dm, 10); run(1000); brightest("drag-alarm-night");
    return 0;
}
