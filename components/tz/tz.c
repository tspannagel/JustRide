#include "tz.h"

#include <ctype.h>
#include <string.h>

const tz_preset_t tz_presets[] = {
    {"UTC", "UTC0"},
    {"London, Lisbon", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Berlin, Paris, Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Athens, Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow, Istanbul", "MSK-3"},
    {"Dubai", "<+04>-4"},
    {"India", "IST-5:30"},
    {"Singapore, Perth", "<+08>-8"},
    {"Tokyo", "JST-9"},
    {"Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Sao Paulo", "<-03>3"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
    {"Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"Phoenix", "MST7"},
    {"Los Angeles", "PST8PDT,M3.2.0,M11.1.0"},
};
const int tz_n_presets = sizeof(tz_presets) / sizeof(tz_presets[0]);

typedef struct {
    char kind;     // 'M', 'J' (1..365, no Feb 29) or 'n' (0..365)
    int m, w, d;   // Mm.w.d; d is the day for J/n
    int32_t time;  // seconds after midnight, may be negative or > 24 h
} rule_t;

typedef struct {
    int32_t std_off, dst_off;  // seconds west of UTC, as written in the rule
    bool has_dst;
    rule_t start, end;
} tz_t;

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
}

static int year_of(int64_t days)
{
    int y = (int)(days / 365) + 1970;
    while (days_from_civil(y, 1, 1) > days) {
        y--;
    }
    while (days_from_civil(y + 1, 1, 1) <= days) {
        y++;
    }
    return y;
}

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static bool number(const char **p, int lo, int hi, int *out)
{
    if (!isdigit((unsigned char)**p)) {
        return false;
    }
    int v = 0;
    while (isdigit((unsigned char)**p) && v <= hi) {
        v = v * 10 + *(*p)++ - '0';
    }
    *out = v;
    return v >= lo && v <= hi;
}

// Zone name: 3+ letters, or <...> with letters, digits, + and -
static bool name(const char **p)
{
    const char *s = *p;
    if (*s == '<') {
        s++;
        const char *start = s;
        while (isalnum((unsigned char)*s) || *s == '+' || *s == '-') {
            s++;
        }
        if (*s != '>' || s - start < 3) {
            return false;
        }
        *p = s + 1;
        return true;
    }
    while (isalpha((unsigned char)*s)) {
        s++;
    }
    if (s - *p < 3) {
        return false;
    }
    *p = s;
    return true;
}

// [+-]hh[:mm[:ss]], hours up to max_h
static bool hms(const char **p, int max_h, int32_t *out)
{
    int sign = 1, h, m = 0, s = 0;
    if (**p == '+' || **p == '-') {
        sign = *(*p)++ == '-' ? -1 : 1;
    }
    if (!number(p, 0, max_h, &h)) {
        return false;
    }
    if (**p == ':') {
        (*p)++;
        if (!number(p, 0, 59, &m)) {
            return false;
        }
        if (**p == ':') {
            (*p)++;
            if (!number(p, 0, 59, &s)) {
                return false;
            }
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return true;
}

static bool rule(const char **p, rule_t *r)
{
    r->time = 2 * 3600;
    if (**p == 'M') {
        (*p)++;
        r->kind = 'M';
        if (!number(p, 1, 12, &r->m) || *(*p)++ != '.' || !number(p, 1, 5, &r->w) || *(*p)++ != '.' ||
            !number(p, 0, 6, &r->d)) {
            return false;
        }
    } else if (**p == 'J') {
        (*p)++;
        r->kind = 'J';
        if (!number(p, 1, 365, &r->d)) {
            return false;
        }
    } else {
        r->kind = 'n';
        if (!number(p, 0, 365, &r->d)) {
            return false;
        }
    }
    if (**p == '/') {
        (*p)++;
        return hms(p, 167, &r->time);
    }
    return true;
}

static bool parse(const char *s, tz_t *tz)
{
    memset(tz, 0, sizeof(*tz));
    if (!s || strlen(s) >= TZ_MAX_LEN || !name(&s) || !hms(&s, 24, &tz->std_off)) {
        return false;
    }
    if (!*s) {
        return true;
    }
    if (!name(&s)) {
        return false;
    }
    tz->has_dst = true;
    tz->dst_off = tz->std_off - 3600;
    if (*s && *s != ',' && !hms(&s, 24, &tz->dst_off)) {
        return false;
    }
    if (!*s) {
        const char *us = ",M3.2.0,M11.1.0";
        s = us;
    }
    return *s++ == ',' && rule(&s, &tz->start) && *s++ == ',' && rule(&s, &tz->end) && !*s;
}

// Seconds from 1970-01-01 00:00 to the rule's day in year y (local wall clock)
static int64_t rule_day(const rule_t *r, int y)
{
    int64_t jan1 = days_from_civil(y, 1, 1);
    if (r->kind == 'J') {
        return (jan1 + r->d - 1 + (is_leap(y) && r->d >= 60)) * 86400;
    }
    if (r->kind == 'n') {
        return (jan1 + r->d) * 86400;
    }
    int64_t first = days_from_civil(y, r->m, 1);
    int wday = (int)((first + 4) % 7);  // 1970-01-01 was a Thursday
    int day = 1 + (r->d - wday + 7) % 7 + (r->w - 1) * 7;
    int next_m = r->m == 12 ? 1 : r->m + 1;
    int64_t month_len = days_from_civil(r->m == 12 ? y + 1 : y, next_m, 1) - first;
    while (day > month_len) {
        day -= 7;  // week 5 = last
    }
    return (first + day - 1) * 86400;
}

bool tz_valid(const char *posix)
{
    tz_t tz;
    return parse(posix, &tz);
}

int32_t tz_offset(const char *posix, uint32_t utc)
{
    tz_t tz;
    if (!parse(posix, &tz)) {
        return 0;
    }
    if (!tz.has_dst) {
        return -tz.std_off;
    }
    int64_t t = utc;
    int y = year_of((t - tz.std_off) / 86400);
    // Start is given in standard time, end in daylight time
    int64_t start = rule_day(&tz.start, y) + tz.start.time + tz.std_off;
    int64_t end = rule_day(&tz.end, y) + tz.end.time + tz.dst_off;
    bool dst = start < end ? t >= start && t < end : !(t >= end && t < start);
    return -(dst ? tz.dst_off : tz.std_off);
}

int tz_preset_index(const char *posix)
{
    for (int i = 0; i < tz_n_presets; i++) {
        if (strcmp(tz_presets[i].posix, posix) == 0) {
            return i;
        }
    }
    return -1;
}
