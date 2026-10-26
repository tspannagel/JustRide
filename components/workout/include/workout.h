// Zwift .zwo parsing and the workout timeline. Pure C.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WORKOUT_MAX_SEGMENTS 200
#define WORKOUT_NAME_LEN 48
#define ZWO_MAX_BYTES (64 * 1024)  // largest .zwo file accepted (upload) and read (start)

typedef enum { SEG_ERG = 0, SEG_FREE = 1 } seg_kind_t;

typedef struct {
    uint16_t dur_s;
    uint16_t p_start;  // per mille of FTP
    uint16_t p_end;
    uint8_t cadence;   // rpm target, 0 = none
    uint8_t kind;      // seg_kind_t
} segment_t;

typedef struct {
    char name[WORKOUT_NAME_LEN];
    uint16_t n;
    uint32_t total_s;
    segment_t seg[WORKOUT_MAX_SEGMENTS];
} workout_t;

// Parse a .zwo document. Returns false if it has no usable segments.
// Extra segments beyond WORKOUT_MAX_SEGMENTS are dropped.
bool zwo_parse(const char *xml, size_t len, workout_t *out);

typedef struct {
    int idx;               // current segment
    uint16_t target_pm;    // per mille of FTP, 0 for free segments
    uint8_t cadence;
    uint8_t kind;          // seg_kind_t
    uint32_t seg_left_s;
    bool done;             // t is past the end
} workout_pos_t;

// Position at workout time t_s.
void workout_at(const workout_t *w, uint32_t t_s, workout_pos_t *pos);

// Workout time at which segment idx starts.
uint32_t workout_seg_start(const workout_t *w, int idx);
