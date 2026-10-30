#include "route.h"

#include <stdlib.h>
#include <string.h>

void route_parse_begin(route_parser_t *p, route_t *r)
{
    memset(r, 0, sizeof(*r));
    *p = (route_parser_t){.r = r};
}

// Whole-line integer within [lo, hi]
static bool parse_int(const char *s, long lo, long hi, long *out)
{
    char *end;
    if (!*s) {
        return false;
    }
    long v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

bool route_parse_line(route_parser_t *p, const char *line)
{
    if (p->bad) {
        return false;
    }
    char buf[80];
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\r') {
        len--;
    }
    if (len >= sizeof(buf)) {
        p->bad = true;
        return false;
    }
    memcpy(buf, line, len);
    buf[len] = 0;
    route_t *r = p->r;
    long v;
    if (p->line++ == 0) {
        p->bad = strcmp(buf, "JustRide route 1") != 0;
    } else if (len == 0) {
        // blank lines are fine
    } else if (strncmp(buf, "name=", 5) == 0) {
        size_t n = len - 5 < ROUTE_NAME_LEN - 1 ? len - 5 : ROUTE_NAME_LEN - 1;
        memcpy(r->name, buf + 5, n);
        r->name[n] = 0;
        for (char *c = r->name; *c; c++) {
            *c = (unsigned char)*c < 0x20 ? ' ' : *c;  // no control characters on screen
        }
    } else if (strncmp(buf, "step_m=", 7) == 0) {
        p->bad = r->n > 0 || !parse_int(buf + 7, ROUTE_STEP_MIN_M, ROUTE_STEP_MAX_M, &v);
        r->step_m = (uint16_t)(p->bad ? 0 : v);
    } else if (r->step_m && r->n < ROUTE_MAX_PTS && parse_int(buf, ROUTE_ELE_MIN_CM, ROUTE_ELE_MAX_CM, &v)) {
        r->ele_cm[r->n++] = (int32_t)v;
    } else {
        p->bad = true;  // unknown line, points before step_m, too many points
    }
    return !p->bad;
}

bool route_parse_end(route_parser_t *p) { return !p->bad && p->line > 0 && p->r->n >= 2; }

uint32_t route_length_m(const route_t *r) { return r->n < 2 ? 0 : (r->n - 1) * r->step_m; }

int32_t route_ele_cm(const route_t *r, uint32_t dist_m)
{
    if (r->n == 0) {
        return 0;
    }
    uint32_t i = dist_m / r->step_m;
    if (i >= r->n - 1) {
        return r->ele_cm[r->n - 1];
    }
    int32_t a = r->ele_cm[i], b = r->ele_cm[i + 1];
    return a + (int32_t)((int64_t)(b - a) * (int32_t)(dist_m % r->step_m) / r->step_m);
}

int16_t route_grade(const route_t *r, uint32_t dist_m)
{
    uint32_t len = route_length_m(r);
    if (len == 0) {
        return 0;
    }
    uint32_t half = ROUTE_GRADE_WINDOW_M / 2;
    uint32_t a = dist_m > half ? dist_m - half : 0, b = dist_m + half;
    // Near the ends: keep the full window inside the route if it fits
    if (b > len) {
        b = len;
        a = len > ROUTE_GRADE_WINDOW_M ? len - ROUTE_GRADE_WINDOW_M : 0;
    } else if (a == 0) {
        b = len < ROUTE_GRADE_WINDOW_M ? len : ROUTE_GRADE_WINDOW_M;
    }
    // cm of height per m of distance is %, times 100 gives 0.01 %
    int64_t g = (int64_t)(route_ele_cm(r, b) - route_ele_cm(r, a)) * 100 / (int64_t)(b - a);
    return (int16_t)(g > ROUTE_GRADE_MAX ? ROUTE_GRADE_MAX : g < -ROUTE_GRADE_MAX ? -ROUTE_GRADE_MAX : g);
}

void route_info(const route_t *r, route_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->length_m = route_length_m(r);
    if (r->n == 0) {
        return;
    }
    out->min_cm = out->max_cm = r->ele_cm[0];
    int64_t climb_cm = 0;
    for (uint32_t i = 1; i < r->n; i++) {
        int32_t e = r->ele_cm[i], d = e - r->ele_cm[i - 1];
        climb_cm += d > 0 ? d : 0;
        out->min_cm = e < out->min_cm ? e : out->min_cm;
        out->max_cm = e > out->max_cm ? e : out->max_cm;
    }
    out->climb_m = (uint32_t)(climb_cm / 100);
    for (uint32_t d = 0; d <= out->length_m; d += r->step_m) {
        int16_t g = route_grade(r, d);
        out->max_grade = g > out->max_grade ? g : out->max_grade;
    }
}
