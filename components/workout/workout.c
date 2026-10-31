#include "workout.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t workout_seg_start(const workout_t *w, int idx)
{
    uint32_t t = 0;
    for (int i = 0; i < idx && i < w->n; i++) {
        t += w->seg[i].dur_s;
    }
    return t;
}

void workout_at(const workout_t *w, uint32_t t_s, workout_pos_t *pos)
{
    memset(pos, 0, sizeof(*pos));
    uint32_t start = 0;
    for (int i = 0; i < w->n; i++) {
        const segment_t *s = &w->seg[i];
        if (t_s < start + s->dur_s) {
            uint32_t in = t_s - start;
            pos->idx = i;
            pos->kind = s->kind;
            pos->cadence = s->cadence;
            pos->goal = s->goal;
            pos->seg_left_s = s->dur_s - in;
            pos->target_pm = (uint16_t)(s->p_start + ((int32_t)s->p_end - s->p_start) * (int32_t)in / s->dur_s);
            return;
        }
        start += s->dur_s;
    }
    pos->idx = w->n > 0 ? w->n - 1 : 0;
    pos->kind = SEG_FREE;
    pos->done = true;
}

uint8_t workout_add_goal(workout_t *w, uint16_t p_pm, goal_t g)
{
    if (g.hr_lo > g.hr_hi) {
        uint8_t t = g.hr_lo;
        g.hr_lo = g.hr_hi;
        g.hr_hi = t;
    }
    if (g.cad_lo > g.cad_hi) {
        uint8_t t = g.cad_lo;
        g.cad_lo = g.cad_hi;
        g.cad_hi = t;
    }
    if (g.hr_pct && g.hr_hi > 100) {
        g.hr_hi = 100;
        g.hr_lo = g.hr_lo < 100 ? g.hr_lo : 100;
    }
    if (g.hr_lo == 0) {
        g.hr_hi = 0;  // a range needs both ends
    }
    if (g.hr_hi == 0) {
        g.hr_pct = false;
    }
    if (g.cad_lo == 0) {
        g.cad_hi = 0;
    }
    if (w->n_goals >= WORKOUT_MAX_GOALS || (!g.hr_hi && !g.cad_hi && !g.hold_avg)) {
        return 0;
    }
    if (g.min_pm == 0) {
        g.min_pm = (uint16_t)(p_pm * 7 / 10);
    }
    if (g.max_pm == 0) {
        g.max_pm = (uint16_t)(p_pm + p_pm * 15 / 100);
    }
    g.min_pm = g.min_pm < 300 ? 300 : g.min_pm;
    if (g.max_pm < g.min_pm) {
        g.max_pm = g.min_pm;
    }
    w->goals[w->n_goals++] = g;
    return w->n_goals;
}

const goal_t *workout_goal(const workout_t *w, int idx)
{
    if (idx < 0 || idx >= w->n || w->seg[idx].goal == 0 || w->seg[idx].goal > w->n_goals) {
        return NULL;
    }
    return &w->goals[w->seg[idx].goal - 1];
}

#define FREE_PM 500  // assumed effort in free segments

uint16_t workout_profile_at(const workout_t *w, uint32_t t_s)
{
    workout_pos_t pos;
    workout_at(w, t_s, &pos);
    if (pos.done) {
        return 0;
    }
    return pos.kind == SEG_FREE ? FREE_PM : pos.target_pm;
}

void workout_estimate(const workout_t *w, workout_est_t *e)
{
    memset(e, 0, sizeof(*e));
    double sum = 0, sum4 = 0;
    uint32_t t = 0;
    for (int i = 0; i < w->n; i++) {
        const segment_t *s = &w->seg[i];
        for (uint32_t k = 0; k < s->dur_s; k++, t++) {
            double p = s->kind == SEG_FREE ? FREE_PM : s->p_start + ((double)s->p_end - s->p_start) * k / s->dur_s;
            sum += p;
            sum4 += p * p * p * p;
            if (p > e->max_pm) {
                e->max_pm = (uint16_t)p;
            }
        }
    }
    e->total_s = t;
    if (t == 0) {
        return;
    }
    e->avg_pm = (uint16_t)(sum / t + 0.5);
    double ifac = pow(sum4 / t, 0.25) / 1000.0;
    e->if_pm = (uint16_t)(ifac * 1000 + 0.5);
    double tss = t / 3600.0 * ifac * ifac * 100;
    e->tss10 = (uint16_t)(tss * 10 < 65535 ? tss * 10 + 0.5 : 65535);
}

void workout_ramp_steps(uint8_t weight_kg, uint16_t *start_w, uint16_t *step_w)
{
    *start_w = (uint16_t)((weight_kg * 3 + 5) / 10 * 5);  // 1.5 W/kg to the nearest 5 W
    *step_w = (uint16_t)((weight_kg + 10) / 20 * 5);      // 0.25 W/kg
    if (*step_w < 10) {
        *step_w = 10;
    }
}

// Per mille of ftp that gives exactly w watts back (pm * ftp / 1000, rounded down)
static uint16_t pm_of(uint32_t w, uint16_t ftp) { return (uint16_t)((w * 1000 + ftp - 1) / ftp); }

void workout_ramp_test(workout_t *w, uint16_t ftp_w, uint8_t weight_kg, uint16_t max_w)
{
    char name[WORKOUT_NAME_LEN];
    snprintf(name, sizeof(name), "%s", w->name[0] ? w->name : "FTP ramp test");
    memset(w, 0, sizeof(*w));
    memcpy(w->name, name, sizeof(name));
    if (ftp_w == 0) {
        return;
    }
    uint16_t start, step;
    workout_ramp_steps(weight_kg, &start, &step);
    uint32_t top = (uint32_t)weight_kg * 7 > 2u * ftp_w ? (uint32_t)weight_kg * 7 : 2u * ftp_w;
    if (top > max_w) {
        top = max_w;
    }
    w->seg[w->n++] = (segment_t){.dur_s = RAMP_WARMUP_S, .p_start = pm_of(start / 2, ftp_w), .p_end = pm_of(start, ftp_w), .kind = SEG_ERG};
    w->ramp_first = (uint8_t)w->n;
    for (uint32_t p = start; (p <= top || w->n == w->ramp_first) && w->n < WORKOUT_MAX_SEGMENTS - 1; p += step) {
        uint16_t pm = pm_of(p, ftp_w);
        w->seg[w->n++] = (segment_t){.dur_s = RAMP_STEP_S, .p_start = pm, .p_end = pm, .kind = SEG_ERG};
    }
    w->ramp_end = (uint8_t)w->n;
    w->seg[w->n++] = (segment_t){.dur_s = RAMP_COOLDOWN_S, .p_start = pm_of(start, ftp_w), .p_end = pm_of(start / 2, ftp_w), .kind = SEG_ERG};
    for (int i = 0; i < w->n; i++) {
        w->total_s += w->seg[i].dur_s;
    }
    w->ramp_test = true;
}
