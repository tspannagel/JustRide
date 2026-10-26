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

int main(void)
{
    test_idle_and_pause_are_low_resistance();
    test_free_ride_gears_and_grade();
    test_workout_erg_targets_and_caps();
    test_settings_are_clamped();
    test_skip_and_stale_data();
    return test_summary("ride");
}
