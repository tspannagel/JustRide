// A route to ride in SIM mode: an elevation profile at fixed distance steps,
// written by the web app from a GPX file (/workouts/<name>.route). Pure C.
//
// File format (text, one item per line):
//   JustRide route 1
//   name=<name>
//   step_m=<5..100>
//   <elevation in cm>      (one per step, from 0 m)
//   ...
//
// The file is untrusted: values are range-checked, and the gradient is taken over
// ROUTE_GRADE_WINDOW_M (noise or a single bad point can't make a wall) and clamped.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROUTE_NAME_LEN 48
#define ROUTE_MAX_PTS 20000         // 200 km at 10 m
#define ROUTE_MAX_BYTES (256 * 1024)
#define ROUTE_STEP_MIN_M 5
#define ROUTE_STEP_MAX_M 100
#define ROUTE_ELE_MIN_CM (-50000)   // -500 m
#define ROUTE_ELE_MAX_CM 900000     // 9000 m
#define ROUTE_GRADE_WINDOW_M 100
#define ROUTE_GRADE_MAX 2500        // 0.01 %; the ride clamps further (vshift)

typedef struct {
    char name[ROUTE_NAME_LEN];
    uint16_t step_m;
    uint32_t n;                     // points; length = (n - 1) * step_m
    int32_t ele_cm[ROUTE_MAX_PTS];
} route_t;

typedef struct {
    route_t *r;
    int line;
    bool bad;
} route_parser_t;

void route_parse_begin(route_parser_t *p, route_t *r);
// One line without the newline (a trailing \r is ignored). False once the file is invalid.
bool route_parse_line(route_parser_t *p, const char *line);
// True if the whole file was a valid route (at least two points)
bool route_parse_end(route_parser_t *p);

uint32_t route_length_m(const route_t *r);
// Elevation at a distance, interpolated; clamped to the ends
int32_t route_ele_cm(const route_t *r, uint32_t dist_m);
// Gradient in 0.01 % around a distance (over ROUTE_GRADE_WINDOW_M), clamped to +-ROUTE_GRADE_MAX
int16_t route_grade(const route_t *r, uint32_t dist_m);

typedef struct {
    uint32_t length_m;
    uint32_t climb_m;               // total ascent
    int32_t min_cm, max_cm;
    int16_t max_grade;              // steepest ROUTE_GRADE_WINDOW_M, 0.01 %
} route_info_t;

void route_info(const route_t *r, route_info_t *out);
