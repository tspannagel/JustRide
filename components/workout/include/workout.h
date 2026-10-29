// Zwift .zwo parsing and the workout timeline. Pure C.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WORKOUT_MAX_SEGMENTS 200
#define WORKOUT_MAX_GOALS 16    // adaptive segments per workout
#define WORKOUT_NAME_LEN 48
#define ZWO_MAX_BYTES (64 * 1024)  // largest .zwo file accepted (upload) and read (start)

typedef enum { SEG_ERG = 0, SEG_FREE = 1 } seg_kind_t;

// Goals of an adaptive segment: the device steers the ERG target within
// [min_pm, max_pm] to meet them (see ride.h). JustRide attributes on a
// SteadyState in the .zwo: JrHrLow, JrHrHigh, JrCadLow, JrCadHigh (bpm/rpm),
// JrAvg="1" (Power is an average to hold), JrMin, JrMax (fractions of FTP).
typedef struct {
    uint8_t hr_lo, hr_hi;    // bpm, 0 = no HR goal
    uint8_t cad_lo, cad_hi;  // rpm, 0 = no cadence goal (cue only)
    uint16_t min_pm, max_pm; // bounds for the target, per mille of FTP
    bool hold_avg;           // the segment's power is an average to hold
} goal_t;

typedef struct {
    uint16_t dur_s;
    uint16_t p_start;  // per mille of FTP
    uint16_t p_end;
    uint8_t cadence;   // rpm target, 0 = none
    uint8_t kind;      // seg_kind_t
    uint8_t goal;      // 1-based index into workout_t.goals, 0 = fixed target
} segment_t;

typedef struct {
    char name[WORKOUT_NAME_LEN];
    uint16_t n;
    uint8_t n_goals;
    uint32_t total_s;
    segment_t seg[WORKOUT_MAX_SEGMENTS];
    goal_t goals[WORKOUT_MAX_GOALS];
} workout_t;

// Adds an adaptive segment's goals; fills in default bounds (start power -30 % /
// +15 %, at least 30 % FTP) and orders the ranges. Returns the 1-based index for
// segment_t.goal, or 0 if the table is full or the goal is empty.
uint8_t workout_add_goal(workout_t *w, uint16_t p_pm, goal_t g);
// Goals of segment idx, NULL for a fixed segment
const goal_t *workout_goal(const workout_t *w, int idx);

// Parse a .zwo document. Returns false if it has no usable segments.
// Extra segments beyond WORKOUT_MAX_SEGMENTS are dropped.
bool zwo_parse(const char *xml, size_t len, workout_t *out);

typedef struct {
    int idx;               // current segment
    uint16_t target_pm;    // per mille of FTP, 0 for free segments
    uint8_t cadence;
    uint8_t kind;          // seg_kind_t
    uint8_t goal;          // segment_t.goal
    uint32_t seg_left_s;
    bool done;             // t is past the end
} workout_pos_t;

// Position at workout time t_s.
void workout_at(const workout_t *w, uint32_t t_s, workout_pos_t *pos);

// Workout time at which segment idx starts.
uint32_t workout_seg_start(const workout_t *w, int idx);

// What a workout will be like if ridden exactly on target (FREE segments count as 50 % FTP).
typedef struct {
    uint32_t total_s;
    uint16_t avg_pm;   // average target, per mille of FTP
    uint16_t max_pm;
    uint16_t if_pm;    // intensity factor x 1000 (4th-power mean of the targets)
    uint16_t tss10;    // TSS x 10
} workout_est_t;

void workout_estimate(const workout_t *w, workout_est_t *e);

// Target at workout time t_s for drawing (FREE segments: 500), 0 past the end.
uint16_t workout_profile_at(const workout_t *w, uint32_t t_s);
