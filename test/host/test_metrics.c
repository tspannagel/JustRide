#include <stdlib.h>
#include "metrics.h"
#include "test.h"

static ride_sample_t *ride(size_t n)
{
    ride_sample_t *s = calloc(n, sizeof(*s));
    for (size_t i = 0; i < n; i++) {
        s[i].t_s = (uint32_t)i;
        s[i].hr_bpm = -1;
        s[i].cadence_rpm10 = 900;
        s[i].speed_kmh100 = 3600; // 36 km/h = 10 m/s
    }
    return s;
}

static void run(const ride_sample_t *s, size_t n, uint16_t ftp, metrics_summary_t *sum)
{
    metrics_t m;
    metrics_init(&m);
    for (size_t i = 0; i < n; i++) {
        metrics_add(&m, &s[i]);
    }
    metrics_summary(&m, ftp, sum);
}

static void test_one_hour_at_ftp(void)
{
    // Definition check: 1 h at exactly FTP -> NP = FTP, IF 1.0, TSS 100
    size_t n = 3600;
    ride_sample_t *s = ride(n);
    for (size_t i = 0; i < n; i++) {
        s[i].power_w = 250;
        s[i].hr_bpm = 140 + (i % 2) * 10;
    }
    metrics_summary_t sum;
    run(s, n, 250, &sum);
    CHECK_EQ(sum.duration_s, 3600);
    CHECK_EQ(sum.avg_w, 250);
    CHECK_EQ(sum.np_w, 250);
    CHECK_EQ(sum.if_1000, 1000);
    CHECK_EQ(sum.tss10, 1000);
    CHECK_EQ(sum.kj, 900);
    CHECK_EQ(sum.avg_hr, 145);
    CHECK_EQ(sum.max_hr, 150);
    CHECK_EQ(sum.avg_cad10, 900);
    CHECK_EQ(sum.distance_m, 36000);
    free(s);
}

static void test_intervals_np_above_avg(void)
{
    // 1 h of 60 s at 400 W / 60 s at 100 W: avg 250. Rolling 30 s average is a
    // trapezoid wave: 30 s at 400, 30 s ramp, 30 s at 100, 30 s ramp.
    // Mean of x^4 = 0.25*400^4 + 0.25*100^4 + 0.5*(400^5-100^5)/(5*300) -> NP ~ 315 W.
    size_t n = 3600;
    ride_sample_t *s = ride(n);
    for (size_t i = 0; i < n; i++) {
        s[i].power_w = (i / 60) % 2 ? 100 : 400;
    }
    metrics_summary_t sum;
    run(s, n, 250, &sum);
    CHECK_EQ(sum.avg_w, 250);
    CHECK(sum.np_w >= 313 && sum.np_w <= 317);
    CHECK_EQ(sum.max_w, 400);
    free(s);
}

static void test_unknown_and_stopped(void)
{
    size_t n = 100;
    ride_sample_t *s = ride(n);
    for (size_t i = 0; i < n; i++) {
        s[i].power_w = i < 50 ? 200 : -1;
        s[i].cadence_rpm10 = i < 50 ? 800 : 0;
        s[i].speed_kmh100 = -1;
    }
    metrics_summary_t sum;
    run(s, n, 0, &sum);
    CHECK_EQ(sum.moving_s, 50);
    CHECK_EQ(sum.avg_w, 100);
    CHECK_EQ(sum.avg_cad10, 800);
    CHECK_EQ(sum.distance_m, 0);
    CHECK_EQ(sum.tss10, 0); // no FTP: no IF/TSS
    CHECK_EQ(sum.avg_hr, 0);
    free(s);
}

static void test_bests(void)
{
    size_t n = 400;
    ride_sample_t *s = ride(n);
    for (size_t i = 0; i < n; i++) {
        s[i].power_w = 100;
    }
    s[200].power_w = 1000;            // 1 s spike
    for (size_t i = 100; i < 110; i++) {
        s[i].power_w = 500;           // 10 s effort
    }
    int16_t b[METRICS_N_BESTS];
    metrics_bests(s, n, b);
    CHECK_EQ(b[0], 1000);             // 1 s
    CHECK_EQ(b[1], 500);              // 5 s
    CHECK_EQ(b[2], 500);              // 10 s
    CHECK_EQ(b[6], (300 * 100 + 10 * 400 + 900) / 300); // 300 s window holds both
    CHECK_EQ(b[7], -1);               // 600 s: ride too short
    CHECK_EQ(metrics_ftp_estimate(b), 0);
    b[8] = 263;                       // 20 min
    CHECK_EQ(metrics_ftp_estimate(b), 250);
    b[4] = 333;                       // 1 min
    CHECK_EQ(metrics_ramp_ftp(b), 250);  // 75 % of the best minute
    b[4] = -1;
    CHECK_EQ(metrics_ramp_ftp(b), 0);
    free(s);
}

static void test_zones(void)
{
    CHECK_EQ(metrics_zone(-1, 200), -1);
    CHECK_EQ(metrics_zone(0, 200), 0);
    CHECK_EQ(metrics_zone(109, 200), 0);   // 54.5 %
    CHECK_EQ(metrics_zone(110, 200), 1);   // 55 %
    CHECK_EQ(metrics_zone(209, 200), 3);
    CHECK_EQ(metrics_zone(210, 200), 4);   // 105 %
    CHECK_EQ(metrics_zone(299, 200), 5);
    CHECK_EQ(metrics_zone(300, 200), 6);   // 150 %
    CHECK_EQ(metrics_zone(1500, 200), 6);
}

static void test_hr_zones_and_peak(void)
{
    CHECK_EQ(metrics_hr_zone(-1, 190), -1);
    CHECK_EQ(metrics_hr_zone(100, 0), -1);
    CHECK_EQ(metrics_hr_zone(113, 190), 0);  // 59.5 %
    CHECK_EQ(metrics_hr_zone(114, 190), 1);  // 60 %
    CHECK_EQ(metrics_hr_zone(133, 190), 2);  // 70 %
    CHECK_EQ(metrics_hr_zone(171, 190), 4);  // 90 %
    CHECK_EQ(metrics_hr_zone(200, 190), 4);  // above max: still the top zone
    uint8_t lo, hi;
    metrics_hr_zone_pct(1, &lo, &hi);
    CHECK_EQ(lo, 60);
    CHECK_EQ(hi, 70);
    metrics_hr_zone_pct(9, &lo, &hi);
    CHECK_EQ(hi, 100);
    CHECK_EQ(metrics_hr_of_pct(60, 185), 111);  // 111.0
    CHECK_EQ(metrics_hr_of_pct(70, 185), 130);  // 129.5 rounds up

    // Peak: one bad reading isn't a peak, 10 s in a row are; a gap restarts the window
    metrics_t m;
    metrics_init(&m);
    ride_sample_t s = {.power_w = 150, .cadence_rpm10 = 900, .speed_kmh100 = -1, .hr_bpm = 150};
    for (int i = 0; i < 30; i++) {
        s.hr_bpm = i == 5 ? 230 : 150;  // a spike
        metrics_add(&m, &s);
    }
    metrics_summary_t sum;
    metrics_summary(&m, 200, &sum);
    CHECK_EQ(sum.max_hr, 230);
    CHECK_EQ(sum.max_hr_peak, 158);  // (9 x 150 + 230) / 10
    for (int i = 0; i < 9; i++) {
        s.hr_bpm = 190;
        metrics_add(&m, &s);
    }
    s.hr_bpm = -1;
    metrics_add(&m, &s);
    s.hr_bpm = 190;
    for (int i = 0; i < 9; i++) {
        metrics_add(&m, &s);
    }
    metrics_summary(&m, 200, &sum);
    CHECK_EQ(sum.max_hr_peak, 186);  // 150 + 9 x 190 in one window; never 10 x 190 across the gap
    metrics_add(&m, &s);
    metrics_summary(&m, 200, &sum);
    CHECK_EQ(sum.max_hr_peak, 190);
}

int main(void)
{
    test_zones();
    test_hr_zones_and_peak();
    test_one_hour_at_ftp();
    test_intervals_np_above_avg();
    test_unknown_and_stopped();
    test_bests();
    return test_summary("metrics");
}
