#include <stdio.h>
#include "ride.h"
#include "test.h"

static ride_t r;
static ride_config_t cfg = {.ftp_w = 200, .max_erg_w = 400, .crr = 40, .cw = 51};

static metrics_evt_t trainer(int power, int cad10)
{
    return (metrics_evt_t){.power_w = (int16_t)power, .cadence_rpm10 = (int16_t)cad10, .speed_kmh100 = 3000,
                           .hr_bpm = -1, .source = SRC_TRAINER};
}

// Run n seconds at 4 ticks/s with steady trainer data. Returns samples taken.
static int ride_for(uint32_t *now, int seconds, int power, int *beeps_total)
{
    int samples = 0;
    for (int i = 0; i < seconds * 4; i++) {
        *now += 250;
        metrics_evt_t e = trainer(power, 900);
        ride_on_metrics(&r, &e, *now);
        ride_sample_t s;
        uint8_t beeps;
        if (ride_tick(&r, *now, 1000000 + *now / 1000, &s, &beeps)) {
            samples++;
        }
        if (beeps_total) {
            *beeps_total += beeps;
        }
    }
    return samples;
}

static void test_idle_and_pause_are_low_resistance(void)
{
    control_cmd_t c;
    ride_init(&r, &cfg);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_RESISTANCE);
    CHECK_EQ(c.level, 0);

    uint32_t now = 1000;
    ride_start(&r, NULL, now, 0);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_SIM);
    ride_action(&r, ACT_PAUSE_TOGGLE);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_RESISTANCE);
    CHECK_EQ(ride_for(&now, 5, 200, NULL), 0); // timer stops while paused
    ride_action(&r, ACT_PAUSE_TOGGLE);
    CHECK_EQ(ride_for(&now, 5, 200, NULL), 5);
    ride_action(&r, ACT_STOP);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_RESISTANCE);
    CHECK_EQ(r.state, RIDE_DONE);
}

static void test_free_ride_gears_and_grade(void)
{
    control_cmd_t c;
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, NULL, now, 0);
    ride_command(&r, &c);
    int16_t neutral = c.grade;
    CHECK(neutral >= -5 && neutral <= 5); // neutral gear ~ real ratio: ~0 %

    ride_action(&r, ACT_GRADE_PLUS);
    ride_action(&r, ACT_GRADE_PLUS);
    for (int i = 0; i < 40; i++) {
        ride_action(&r, ACT_PLUS); // past the top gear
    }
    ride_command(&r, &c);
    CHECK_EQ(r.gear, 23);
    CHECK(c.grade > 100);
    CHECK(c.grade <= VSHIFT_GRADE_MAX);
    for (int i = 0; i < 100; i++) {
        ride_action(&r, ACT_GRADE_PLUS);
    }
    ride_command(&r, &c);
    CHECK_EQ(c.grade, VSHIFT_GRADE_MAX);
}

static void test_workout_erg_targets_and_caps(void)
{
    static workout_t w = {.n = 3, .total_s = 30,
                          .seg = {{.dur_s = 10, .p_start = 500, .p_end = 500},
                                  {.dur_s = 10, .p_start = 3000, .p_end = 3000}, // 600 W at FTP 200
                                  {.dur_s = 10, .kind = SEG_FREE}}};
    control_cmd_t c;
    int beeps = 0;
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, &w, now, 0);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_ERG);
    CHECK_EQ(c.target_w, 100);

    ride_action(&r, ACT_PLUS); // intensity +5 %
    ride_command(&r, &c);
    CHECK_EQ(c.target_w, 105);
    for (int i = 0; i < 50; i++) {
        ride_action(&r, ACT_PLUS);
    }
    CHECK_EQ(r.intensity, RIDE_INTENSITY_MAX);
    for (int i = 0; i < 50; i++) {
        ride_action(&r, ACT_MINUS);
    }
    CHECK_EQ(r.intensity, RIDE_INTENSITY_MIN);
    r.intensity = 100;

    ride_for(&now, 11, 100, &beeps);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_ERG);
    CHECK_EQ(c.target_w, 400); // 600 W capped by max_erg_w

    ride_for(&now, 10, 100, &beeps);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_SIM); // free segment

    ride_for(&now, 10, 100, &beeps);
    CHECK(r.wo_done);
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_SIM);
    CHECK_EQ(beeps, 1 + 2 + 1 + 2 + 3); // 3 s warnings, changes, done
}

static void test_settings_are_clamped(void)
{
    ride_config_t bad = {.ftp_w = 5000, .max_erg_w = 9999};
    control_cmd_t c;
    static workout_t w = {.n = 1, .total_s = 10, .seg = {{.dur_s = 10, .p_start = 3000, .p_end = 3000}}};
    ride_init(&r, &bad);
    CHECK_EQ(r.cfg.ftp_w, RIDE_FTP_MAX);
    ride_start(&r, &w, 1000, 0);
    ride_command(&r, &c);
    CHECK_EQ(c.target_w, RIDE_ERG_CAP_W);
}

static void test_skip_and_stale_data(void)
{
    static workout_t w = {.n = 2, .total_s = 120,
                          .seg = {{.dur_s = 60, .p_start = 500, .p_end = 500},
                                  {.dur_s = 60, .p_start = 1000, .p_end = 1000}}};
    control_cmd_t c;
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, &w, now, 0);
    ride_for(&now, 5, 100, NULL);
    ride_action(&r, ACT_SKIP);
    ride_command(&r, &c);
    CHECK_EQ(c.target_w, 200);

    // Trainer goes silent: samples must say "unknown", not repeat old values
    ride_sample_t s = {0};
    uint8_t beeps;
    for (int i = 0; i < 20; i++) {
        now += 250;
        ride_tick(&r, now, 0, &s, &beeps);
    }
    CHECK_EQ(s.power_w, -1);
    CHECK_EQ(s.cadence_rpm10, -1);
}

// Data arriving between two ticks is newer than the ride's clock: it must show, not
// read as stale until the next tick (cadence and HR flickered "-" on the bike)
static void test_data_between_ticks_is_fresh(void)
{
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, NULL, now, 0);
    ride_for(&now, 5, 150, NULL);
    metrics_evt_t e = trainer(160, 850);
    ride_on_metrics(&r, &e, now + 100);
    metrics_evt_t h = {.power_w = -1, .cadence_rpm10 = -1, .speed_kmh100 = -1, .hr_bpm = 120, .source = SRC_HR};
    ride_on_metrics(&r, &h, now + 100);
    ride_view_t v;
    ride_view(&r, &v);
    CHECK_EQ(v.cadence_rpm10, 850);
    CHECK_EQ(v.speed_kmh100, 3000);
    CHECK_EQ(v.hr_bpm, 120);
    CHECK(v.power_w > 0);
}

// n ticks of 250 ms with the given cadence and speed
static void ticks(uint32_t *now, int n, int cad10, int speed)
{
    for (int i = 0; i < n; i++) {
        *now += 250;
        metrics_evt_t e = {.power_w = cad10 ? 150 : 0, .cadence_rpm10 = (int16_t)cad10, .speed_kmh100 = speed,
                           .hr_bpm = -1, .source = SRC_TRAINER};
        ride_on_metrics(&r, &e, *now);
        ride_sample_t s;
        uint8_t beeps;
        ride_tick(&r, *now, 0, &s, &beeps);
    }
}

static void test_auto_pause(void)
{
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, NULL, now, 0);
    ticks(&now, 20, 900, 3000);
    CHECK_EQ(r.state, RIDE_RUNNING);

    // Coasting: no cadence, but the flywheel still turns
    ticks(&now, 20, 0, 2000);
    CHECK_EQ(r.state, RIDE_RUNNING);
    // Stopped: pauses after 3 s, timer stands still, lowest resistance
    ticks(&now, 11, 0, 300);
    CHECK_EQ(r.state, RIDE_RUNNING);
    ticks(&now, 2, 0, 300);
    CHECK_EQ(r.state, RIDE_PAUSED);
    ride_view_t v;
    ride_view(&r, &v);
    CHECK(v.auto_paused);
    uint32_t t = r.timer_ms;
    control_cmd_t c;
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_RESISTANCE);
    ticks(&now, 40, 0, 0);
    CHECK_EQ(r.timer_ms, t);

    // A few slow turns don't resume; 2 s of pedalling does
    ticks(&now, 4, 200, 500);
    CHECK_EQ(r.state, RIDE_PAUSED);
    ticks(&now, 9, 800, 1500);
    CHECK_EQ(r.state, RIDE_RUNNING);
    ride_view(&r, &v);
    CHECK(!v.auto_paused);

    // Trainer gone: no auto-pause on stale data
    for (int i = 0; i < 40; i++) {
        now += 250;
        ride_sample_t s;
        uint8_t beeps;
        ride_tick(&r, now, 0, &s, &beeps);
    }
    CHECK_EQ(r.state, RIDE_RUNNING);

    // A tapped pause doesn't resume by pedalling
    ticks(&now, 4, 900, 3000);
    ride_action(&r, ACT_PAUSE_TOGGLE);
    ticks(&now, 40, 900, 3000);
    CHECK_EQ(r.state, RIDE_PAUSED);
    ride_action(&r, ACT_PAUSE_TOGGLE);
    CHECK_EQ(r.state, RIDE_RUNNING);
}

static void test_lap_and_interval_feedback(void)
{
    static workout_t w = {.n = 2, .total_s = 120,
                          .seg = {{.dur_s = 60, .p_start = 500, .p_end = 500},
                                  {.dur_s = 60, .p_start = 1000, .p_end = 1000}}};
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, &w, now, 0);
    ride_view_t v;
    ride_for(&now, 30, 150, NULL);
    ride_view(&r, &v);
    CHECK_EQ(v.last_blk.kind, BLOCK_NONE);
    CHECK_EQ(v.zone, 2);  // 150 W of 200 = 75 %: tempo
    CHECK(v.zone_s >= 28 && v.zone_s <= 30);

    ride_action(&r, ACT_LAP);
    ride_view(&r, &v);
    CHECK_EQ(v.last_blk.kind, BLOCK_LAP);
    CHECK_EQ(v.last_blk.no, 1);
    CHECK_EQ(v.last_blk.avg_w, 150);
    CHECK(v.last_blk.s >= 29 && v.last_blk.s <= 30);

    // The first interval ends at 60 s
    ride_for(&now, 31, 250, NULL);
    ride_view(&r, &v);
    CHECK_EQ(v.last_blk.kind, BLOCK_INTERVAL);
    CHECK_EQ(v.last_blk.no, 1);
    CHECK_EQ(v.last_blk.s, 60);
    CHECK(v.last_blk.avg_w > 150 && v.last_blk.avg_w < 250);
    // Shown for RIDE_BLOCK_SHOW_S, then gone
    ride_for(&now, RIDE_BLOCK_SHOW_S + 1, 250, NULL);
    ride_view(&r, &v);
    CHECK_EQ(v.last_blk.kind, BLOCK_NONE);

    // A quick lap press isn't shown
    ride_action(&r, ACT_LAP);
    ride_for(&now, 2, 250, NULL);
    ride_action(&r, ACT_LAP);
    ride_view(&r, &v);
    CHECK_EQ(v.last_blk.no, 2);  // still the earlier lap 2 (12 s), not lap 3
}

// 500 m flat, then 30 % (clamped to +20 % road), 1 km long
static route_t route;
static void steep_route(void)
{
    route_parser_t p;
    route_parse_begin(&p, &route);
    route_parse_line(&p, "JustRide route 1");
    route_parse_line(&p, "step_m=10");
    char line[16];
    for (int d = 0; d <= 1000; d += 10) {
        snprintf(line, sizeof(line), "%d", d < 500 ? 0 : (d - 500) * 30);
        route_parse_line(&p, line);
    }
    CHECK(route_parse_end(&p));
}

static void test_route_ride(void)
{
    steep_route();
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start_route(&r, &route, now, 0);
    ride_view_t v;
    ride_view(&r, &v);
    CHECK(v.route);
    CHECK_EQ(v.route_len_m, 1000);
    CHECK_EQ(v.base_grade, 0);
    ride_action(&r, ACT_GRADE_PLUS);  // ignored: the route sets it
    ride_view(&r, &v);
    CHECK_EQ(v.base_grade, 0);

    // 30 km/h = 8.33 m/s: 72 s to 600 m
    ride_for(&now, 72, 200, NULL);
    ride_view(&r, &v);
    CHECK(v.route_dist_m >= 590 && v.route_dist_m <= 610);
    CHECK_EQ(v.base_grade, VSHIFT_GRADE_MAX);  // 30 % clamped to 20 %
    control_cmd_t c;
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_SIM);
    CHECK(c.grade <= VSHIFT_GRADE_MAX);

    // Past the end: flat, 3 beeps once
    int beeps = 0;
    ride_for(&now, 60, 200, &beeps);
    ride_view(&r, &v);
    CHECK(v.route_done);
    CHECK_EQ(v.base_grade, 0);
    CHECK(beeps >= 3);
}

// No auto-pause in the ramp of a ramp test: its own rule ends the ramp
static void test_no_auto_pause_in_ramp(void)
{
    static workout_t rt;
    workout_ramp_test(&rt, 200, 75, 400);
    ride_init(&r, &cfg);
    uint32_t now = 1000;
    ride_start(&r, &rt, now, 0);
    r.wo_offset_s = (int32_t)workout_seg_start(&rt, rt.ramp_first);
    ticks(&now, 8, 900, 3000);
    ticks(&now, 30, 0, 0);
    CHECK_EQ(r.state, RIDE_RUNNING);
}

// n seconds at 4 ticks/s with trainer data and a heart rate (hr < 0: strap silent)
static int ride_hr(uint32_t *now, int seconds, int power, int cad10, int hr)
{
    int beeps_total = 0;
    for (int i = 0; i < seconds * 4; i++) {
        *now += 250;
        metrics_evt_t e = trainer(power, cad10);
        ride_on_metrics(&r, &e, *now);
        if (hr > 0) {
            metrics_evt_t h = {.power_w = -1, .cadence_rpm10 = -1, .speed_kmh100 = -1, .hr_bpm = (int16_t)hr, .source = SRC_HR};
            ride_on_metrics(&r, &h, *now);
        }
        ride_sample_t s;
        uint8_t beeps;
        ride_tick(&r, *now, 0, &s, &beeps);
        beeps_total += beeps;
    }
    return beeps_total;
}

static int erg_target(void)
{
    control_cmd_t c;
    ride_command(&r, &c);
    CHECK_EQ(c.kind, CMD_ERG);
    return c.target_w;
}

// One adaptive segment: 65 % of FTP 200 = 130 W, bounds 91..149 W
static workout_t awo;
static void adaptive_workout(goal_t g)
{
    memset(&awo, 0, sizeof(awo));
    awo.n = 1;
    awo.seg[0] = (segment_t){.dur_s = 3600, .p_start = 650, .p_end = 650, .kind = SEG_ERG};
    awo.seg[0].goal = workout_add_goal(&awo, 650, g);
    awo.total_s = 3600;
}

static void test_adaptive_hr(void)
{
    uint32_t now = 1000;
    adaptive_workout((goal_t){.hr_lo = 140, .hr_hi = 150});
    ride_init(&r, &cfg);

    // In range: the planned target stays
    ride_start(&r, &awo, now, 0);
    ride_hr(&now, 60, 130, 900, 145);
    CHECK_EQ(erg_target(), 130);

    // More than 5 bpm too high: 10 W per 20 s, down to the lower bound and no further
    ride_hr(&now, 20, 130, 900, 160);
    CHECK_EQ(erg_target(), 120);
    ride_hr(&now, 300, 130, 900, 160);
    CHECK_EQ(erg_target(), 91);

    // Too low: 5 W per 20 s up to the upper bound
    ride_start(&r, &awo, now, 0);
    ride_hr(&now, 40, 130, 900, 120);
    CHECK_EQ(erg_target(), 140);
    ride_hr(&now, 600, 130, 900, 120);
    CHECK_EQ(erg_target(), 149);

    // Strap lost: never higher; after a minute back towards the plan
    ride_hr(&now, 300, 130, 900, -1);
    CHECK_EQ(erg_target(), 130);

    // Max ERG wins over the segment's bounds
    ride_config_t low = cfg;
    low.max_erg_w = 110;
    ride_init(&r, &low);
    ride_start(&r, &awo, now, 0);
    CHECK_EQ(erg_target(), 110);
    ride_hr(&now, 300, 110, 900, 120);
    CHECK_EQ(erg_target(), 110);
}

static void test_adaptive_hold_average(void)
{
    uint32_t now = 1000;
    adaptive_workout((goal_t){.hold_avg = true});
    awo.seg[0].dur_s = 600;  // a 10 min part: the shortfall is made up over the other 5 min
    awo.total_s = 600;
    ride_init(&r, &cfg);
    ride_start(&r, &awo, now, 0);
    // Rider fell short for 5 min (e.g. soft pedalling): the target goes above 130 to make it up
    ride_hr(&now, 300, 100, 900, -1);
    CHECK_EQ(erg_target(), 149);  // 160 W would be needed; capped at the upper bound
    ride_view_t v;
    ride_view(&r, &v);
    CHECK(v.adaptive && v.hold_avg);
    CHECK_EQ(v.avg_goal_w, 130);
    CHECK_EQ(v.seg_avg_w, 100);

    // With an HR range, too-high HR caps it below the plan despite the shortfall
    adaptive_workout((goal_t){.hold_avg = true, .hr_lo = 140, .hr_hi = 150});
    ride_start(&r, &awo, now, 0);
    ride_hr(&now, 120, 100, 900, 165);
    CHECK(erg_target() < 130);
    CHECK(erg_target() >= 91);
}

static void test_adaptive_hold_average_stop_and_rise(void)
{
    uint32_t now = 1000;
    adaptive_workout((goal_t){.hold_avg = true, .max_pm = 1200});  // up to 240 W
    awo.seg[0].dur_s = 600;
    awo.total_s = 600;
    ride_init(&r, &cfg);
    ride_start(&r, &awo, now, 0);
    ride_hr(&now, 480, 130, 900, -1);
    CHECK_EQ(erg_target(), 130);

    // Stopped for 30 s (drinking) two minutes before the end: no shortfall, no jump
    ride_hr(&now, 30, 0, 0, -1);
    CHECK_EQ(erg_target(), 130);
    ride_hr(&now, 10, 130, 900, -1);
    CHECK_EQ(erg_target(), 130);

    // A real shortfall near the end: the target rises 5 W per 10 s, not at once
    ride_hr(&now, 30, 100, 900, -1);
    int t = erg_target();
    CHECK(t > 130 && t <= 145);
    ride_hr(&now, 10, 100, 900, -1);
    CHECK(erg_target() - t <= 5);
}

static void test_adaptive_cadence_cue(void)
{
    uint32_t now = 1000;
    adaptive_workout((goal_t){.cad_lo = 85, .cad_hi = 95});
    ride_init(&r, &cfg);
    ride_start(&r, &awo, now, 0);
    CHECK_EQ(ride_hr(&now, 30, 130, 900, -1), 0);
    // Too slow for 5 s: cue and one beep, not one per second
    CHECK_EQ(ride_hr(&now, 30, 130, 700, -1), 1);
    ride_view_t v;
    ride_view(&r, &v);
    CHECK_EQ(v.cad_cue, -1);
    CHECK_EQ(erg_target(), 130);  // cadence never changes the power
    ride_hr(&now, 5, 130, 900, -1);
    ride_view(&r, &v);
    CHECK_EQ(v.cad_cue, 0);
}

// FTP ramp test, 80 kg, FTP 200, Max ERG 400: warm-up, 120 W + 20 W/min up to 400 W, cool-down
static workout_t rt;

static void test_ramp_test_ends_when_rider_cannot_hold_it(void)
{
    workout_ramp_test(&rt, 200, 80, 400);
    uint32_t now = 1000;
    ride_init(&r, &cfg);
    ride_start(&r, &rt, now, 0);
    ride_hr(&now, RAMP_WARMUP_S + 5 * RAMP_STEP_S, 400, 900, -1);
    ride_view_t v;
    ride_view(&r, &v);
    CHECK(v.ramp_test);
    CHECK_EQ(v.ramp_step, 6);
    CHECK_EQ(v.ramp_s, 5 * RAMP_STEP_S + 1);  // a sample holds the target at its end
    CHECK_EQ(erg_target(), 220);
    ride_action(&r, ACT_PLUS);  // no intensity change during a test
    CHECK_EQ(erg_target(), 220);

    // Cadence below 60 rpm: the ramp ends after 10 s, with 3 beeps, into the cool-down
    CHECK_EQ(ride_hr(&now, RAMP_FAIL_S - 1, 220, 500, -1), 0);
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 6);
    CHECK_EQ(ride_hr(&now, 1, 220, 500, -1), 3);
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 0);
    CHECK_EQ(v.seg_idx, rt.ramp_end);
    CHECK_EQ(v.ramp_s, 5 * RAMP_STEP_S + RAMP_FAIL_S + 1);
    int t = erg_target();
    CHECK(t <= 120 && t >= 60);
    CHECK_EQ(ride_hr(&now, 5, 50, 500, -1), 0);  // the cool-down is not watched
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_s, 5 * RAMP_STEP_S + RAMP_FAIL_S + 1);
}

static void test_ramp_test_power_stale_and_skip(void)
{
    workout_ramp_test(&rt, 200, 80, 400);
    uint32_t now = 1000;
    ride_init(&r, &cfg);
    ride_start(&r, &rt, now, 0);
    ride_hr(&now, 100, 50, 300, -1);  // the warm-up is not watched
    ride_action(&r, ACT_SKIP);        // skips the warm-up only
    ride_view_t v;
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 1);
    CHECK_EQ(erg_target(), 120);

    // Power below 80 % of the target ends it too; a short dip doesn't
    ride_hr(&now, 5, 90, 900, -1);
    ride_hr(&now, 1, 120, 900, -1);
    ride_hr(&now, 4, 90, 900, -1);
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 1);  // 4 low seconds so far
    ride_hr(&now, RAMP_FAIL_S - 5, 90, 900, -1);
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 1);
    ride_hr(&now, 1, 90, 900, -1);
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 0);
    CHECK_EQ(v.seg_idx, rt.ramp_end);

    // The trainer drops out (no data for 3 s, then stale): the ramp ends rather than
    // climbing on without the rider, who would come back to a step never ridden up to
    ride_init(&r, &cfg);
    ride_start(&r, &rt, now, 0);
    ride_hr(&now, RAMP_WARMUP_S + 30, 200, 900, -1);
    for (int i = 0; i < 4 * (3 + RAMP_FAIL_S - 1); i++) {
        now += 250;
        ride_sample_t s;
        uint8_t beeps;
        ride_tick(&r, now, 0, &s, &beeps);
    }
    ride_view(&r, &v);
    CHECK(v.ramp_step > 0);  // last 3 s still fresh at 200 W: 9 stale seconds so far
    for (int i = 0; i < 4 * 2; i++) {
        now += 250;
        ride_sample_t s;
        uint8_t beeps;
        ride_tick(&r, now, 0, &s, &beeps);
    }
    ride_view(&r, &v);
    CHECK_EQ(v.ramp_step, 0);
    CHECK_EQ(v.seg_idx, rt.ramp_end);

    // Skip during the ramp ends the test
    ride_init(&r, &cfg);
    ride_start(&r, &rt, now, 0);
    ride_hr(&now, RAMP_WARMUP_S + 30, 200, 900, -1);
    ride_action(&r, ACT_SKIP);
    ride_view(&r, &v);
    CHECK_EQ(v.seg_idx, rt.ramp_end);
    CHECK_EQ(v.ramp_s, 31);
}

int main(void)
{
    test_idle_and_pause_are_low_resistance();
    test_free_ride_gears_and_grade();
    test_workout_erg_targets_and_caps();
    test_settings_are_clamped();
    test_skip_and_stale_data();
    test_data_between_ticks_is_fresh();
    test_auto_pause();
    test_no_auto_pause_in_ramp();
    test_lap_and_interval_feedback();
    test_route_ride();
    test_adaptive_hr();
    test_adaptive_hold_average();
    test_adaptive_hold_average_stop_and_rise();
    test_adaptive_cadence_cue();
    test_ramp_test_ends_when_rider_cannot_hold_it();
    test_ramp_test_power_stale_and_skip();
    return test_summary("ride");
}
