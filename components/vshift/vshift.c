#include "vshift.h"

#include <math.h>
#include <stdlib.h>

#define DEFAULT_GEARS 24
#define DEFAULT_LOW 0.75
#define DEFAULT_HIGH 5.5

void vshift_default(gear_table_t *g)
{
    g->n = DEFAULT_GEARS;
    g->real_ratio100 = 200;
    double step = pow(DEFAULT_HIGH / DEFAULT_LOW, 1.0 / (DEFAULT_GEARS - 1));
    for (int i = 0; i < DEFAULT_GEARS; i++) {
        g->ratio100[i] = (uint16_t)lround(DEFAULT_LOW * pow(step, i) * 100);
    }
}

int vshift_neutral_gear(const gear_table_t *g)
{
    int best = 0;
    for (int i = 1; i < g->n; i++) {
        if (abs(g->ratio100[i] - g->real_ratio100) < abs(g->ratio100[best] - g->real_ratio100)) {
            best = i;
        }
    }
    return best;
}

int16_t vshift_grade(const gear_table_t *g, int gear, int16_t grade, uint8_t crr)
{
    if (g->n == 0 || g->real_ratio100 == 0) {
        return grade;
    }
    gear = gear < 0 ? 0 : gear >= g->n ? g->n - 1 : gear;
    double k = (double)g->ratio100[gear] / g->real_ratio100;
    double cmd = (grade + crr) * k - crr; // both in 0.01 % / 0.0001 units: same scale
    long v = lround(cmd);
    return (int16_t)(v < VSHIFT_GRADE_MIN ? VSHIFT_GRADE_MIN : v > VSHIFT_GRADE_MAX ? VSHIFT_GRADE_MAX : v);
}
