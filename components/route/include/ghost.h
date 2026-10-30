// Your best earlier ride on a route, to race against: its distance after every
// second, rebuilt from the ride's per-second CSV (speed, as the recorder's
// distance). Pure C.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"

#define GHOST_MAX_S 21600  // 6 h

typedef struct {
    uint32_t n;                       // seconds
    uint64_t mm;                      // running distance
    uint32_t dist_dm[GHOST_MAX_S];    // distance after second i + 1, 0.1 m
} ghost_t;

void ghost_begin(ghost_t *g);
// The ride's samples in order; false once full (the rest is ignored)
bool ghost_add(ghost_t *g, const ride_sample_t *s);
// Distance after t seconds of riding (clamped to the ride's end)
uint32_t ghost_dist_m(const ghost_t *g, uint32_t t_s);
// Seconds the ride needed to cover dist_m, -1 if it never got that far
int32_t ghost_time_at(const ghost_t *g, uint32_t dist_m);

// One row of summary.csv: ride id, workout (route) name, duration and distance.
// False for the header or a malformed row.
typedef struct {
    char ride_id[24];
    char workout[64];
    uint32_t duration_s;
    uint32_t distance_m;
} ghost_summary_t;

bool ghost_summary_row(const char *line, ghost_summary_t *out);
