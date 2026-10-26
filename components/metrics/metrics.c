#include "metrics.h"

#include <math.h>
#include <string.h>

const uint16_t metrics_best_s[METRICS_N_BESTS] = {1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600};

static uint16_t power_of(const ride_sample_t *s) { return s->power_w > 0 ? (uint16_t)s->power_w : 0; }

void metrics_init(metrics_t *m) { memset(m, 0, sizeof(*m)); }

void metrics_add(metrics_t *m, const ride_sample_t *s)
{
    uint16_t p = power_of(s);
    int16_t cad = s->cadence_rpm10;

    // NP: 30 s rolling average, mean of its 4th power, 4th root
    uint32_t slot = m->n % NP_WINDOW_S;
    m->ring_sum += p - m->ring[slot];
    m->ring[slot] = p;
    m->n++;
    if (m->n >= NP_WINDOW_S) {
        double avg = (double)m->ring_sum / NP_WINDOW_S;
        m->sum_np4 += avg * avg * avg * avg;
        m->n_np++;
    }

    m->sum_power += p;
    if ((int16_t)p > m->max_power) {
        m->max_power = (int16_t)p;
    }
    if (p > 0 || cad > 0) {
        m->moving_s++;
    }
    if (cad > 0) {
        m->sum_cad10 += (uint32_t)cad;
        m->n_cad++;
    }
    if (s->hr_bpm > 0) {
        m->sum_hr += (uint32_t)s->hr_bpm;
        m->n_hr++;
        if (s->hr_bpm > m->max_hr) {
            m->max_hr = s->hr_bpm;
        }
    }
    if (s->speed_kmh100 > 0) {
        m->distance_mm += (uint64_t)s->speed_kmh100 * 100 / 36; // km/h x100 -> mm per s
    }
}

void metrics_summary(const metrics_t *m, uint16_t ftp_w, metrics_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->duration_s = m->n;
    out->moving_s = m->moving_s;
    out->max_w = m->max_power;
    out->kj = (uint32_t)(m->sum_power / 1000);
    out->distance_m = (uint32_t)(m->distance_mm / 1000);
    if (m->n > 0) {
        out->avg_w = (int16_t)(m->sum_power / m->n);
    }
    if (m->n_np > 0) {
        out->np_w = (int16_t)lround(pow(m->sum_np4 / m->n_np, 0.25));
    }
    if (ftp_w > 0 && out->np_w > 0) {
        double intensity = (double)out->np_w / ftp_w;
        out->if_1000 = (uint16_t)lround(intensity * 1000);
        out->tss10 = (uint16_t)lround(m->n * out->np_w * intensity / (ftp_w * 3600.0) * 1000);
    }
    if (m->n_hr > 0) {
        out->avg_hr = (int16_t)(m->sum_hr / m->n_hr);
        out->max_hr = m->max_hr;
    }
    if (m->n_cad > 0) {
        out->avg_cad10 = (uint16_t)(m->sum_cad10 / m->n_cad);
    }
}

void metrics_bests(const ride_sample_t *s, size_t n, int16_t out[METRICS_N_BESTS])
{
    for (int b = 0; b < METRICS_N_BESTS; b++) {
        size_t w = metrics_best_s[b];
        if (n < w) {
            out[b] = -1;
            continue;
        }
        uint64_t sum = 0, best = 0;
        for (size_t i = 0; i < n; i++) {
            sum += power_of(&s[i]);
            if (i >= w) {
                sum -= power_of(&s[i - w]);
            }
            if (i + 1 >= w && sum > best) {
                best = sum;
            }
        }
        out[b] = (int16_t)(best / w);
    }
}
