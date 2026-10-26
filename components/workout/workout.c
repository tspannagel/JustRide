#include "workout.h"

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
