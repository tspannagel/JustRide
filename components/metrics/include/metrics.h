// Ride metrics from 1 Hz samples. Pure C. The same accumulator runs live and
// at ride end (or over a recovered CSV), so the numbers always match.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"

#define NP_WINDOW_S 30

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
