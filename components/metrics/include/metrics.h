// Ride metrics from 1 Hz samples. Pure C. The same accumulator runs live and
// at ride end (or over a recovered CSV), so the numbers always match.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"

#define NP_WINDOW_S 30
#define METRICS_HR_PEAK_S 10  // max_hr_peak: HR held this long, so one bad reading isn't a peak

typedef struct {
    uint32_t n;            // samples (= timer seconds)
    uint32_t moving_s;
    uint64_t sum_power;
    int16_t max_power;
    uint16_t ring[NP_WINDOW_S];
    uint32_t ring_sum;
    double sum_np4;
    uint32_t n_np;
    uint64_t sum_hr;
    uint32_t n_hr;
    int16_t max_hr;
    uint8_t hr_ring[METRICS_HR_PEAK_S];  // last seconds of known HR, for max_hr_peak
    uint16_t hr_run;                     // seconds in a row with HR
    uint16_t hr_ring_sum;
    int16_t max_hr_peak;
    uint64_t sum_cad10;
    uint32_t n_cad;
    uint64_t distance_mm;
} metrics_t;

typedef struct {
    uint32_t duration_s;
    uint32_t moving_s;
    int16_t avg_w;
    int16_t np_w;
    int16_t max_w;
    uint16_t if_1000;     // intensity factor x 1000
    uint16_t tss10;       // TSS x 10
    uint32_t kj;
    int16_t avg_hr;       // 0 if no HR
    int16_t max_hr;
    int16_t max_hr_peak;  // best METRICS_HR_PEAK_S average with HR in every second, 0 if none
    uint16_t avg_cad10;   // over pedalling seconds
    uint32_t distance_m;
} metrics_summary_t;

void metrics_init(metrics_t *m);
void metrics_add(metrics_t *m, const ride_sample_t *s);
void metrics_summary(const metrics_t *m, uint16_t ftp_w, metrics_summary_t *out);

#define METRICS_N_BESTS 11
extern const uint16_t metrics_best_s[METRICS_N_BESTS]; // 1, 5, 10, ... 3600

// Best average power for each duration in metrics_best_s; -1 if the ride is shorter.
void metrics_bests(const ride_sample_t *s, size_t n, int16_t out[METRICS_N_BESTS]);

// FTP estimate: 95 % of the best 20 min power; 0 if the ride is shorter than 20 min.
uint16_t metrics_ftp_estimate(const int16_t bests[METRICS_N_BESTS]);
// Power zone 0..6 (Coggan, as the web app: < 55, 75, 90, 105, 120, 150 % FTP, above);
// -1 for unknown power
#define METRICS_N_ZONES 7
int metrics_zone(int power_w, uint16_t ftp_w);

// Heart-rate zone 0..4 by % of max HR (as the web app: < 60, 70, 80, 90 %, above);
// -1 for unknown HR
#define METRICS_N_HR_ZONES 5
int metrics_hr_zone(int hr_bpm, uint8_t max_hr);
// Range to ride in heart-rate zone z (0..4), % of max HR: 50-60, 60-70, ... 90-100
void metrics_hr_zone_pct(int z, uint8_t *lo_pct, uint8_t *hi_pct);
// pct % of max HR in bpm, rounded
uint8_t metrics_hr_of_pct(uint8_t pct, uint8_t max_hr);

// FTP from a ramp test: 75 % of the best minute; 0 if the ride is shorter than 1 min.
uint16_t metrics_ramp_ftp(const int16_t bests[METRICS_N_BESTS]);
