// Virtual shifting, strategy A: scale the SIM grade by virtual/real gear ratio.
// Small-angle model, ignores aero; tune by feel. Pure C.
#pragma once

#include <stdint.h>

#define VSHIFT_MAX_GEARS 30
#define VSHIFT_GRADE_MIN (-1000)  // -10.00 %
#define VSHIFT_GRADE_MAX 2000     // +20.00 %

typedef struct {
    uint8_t n;
    uint16_t ratio100[VSHIFT_MAX_GEARS];  // virtual ratios x100, ascending
    uint16_t real_ratio100;               // the chain's real ratio x100 (34x17 = 200)
} gear_table_t;

// 24 gears, geometric 0.75 .. 5.5, real ratio 2.00.
void vshift_default(gear_table_t *g);

// Index of the gear closest to the real ratio (feels like no virtual shifting).
int vshift_neutral_gear(const gear_table_t *g);

// grade_cmd = (grade + Crr) * k - Crr, k = virtual / real, clamped -10..+20 %.
// grade in 0.01 %, crr in 0.0001. Returns the SIM grade in 0.01 %.
int16_t vshift_grade(const gear_table_t *g, int gear, int16_t grade, uint8_t crr);
