// Minimal .zwo reader: a tag scanner, not a full XML parser. Handles the
// elements Zwift writes; unknown elements and text events are skipped.

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "workout.h"

#define MAX_DUR_S 36000
#define MAX_POWER_PM 3000

typedef struct {
    double duration, power, power_low, power_high;
    double cadence, cadence_resting;
    double repeat, on_duration, off_duration, on_power, off_power;
} attrs_t;

static const char *find_comment_end(const char *p, const char *end)
{
    for (; p + 3 <= end; p++) {
        if (p[0] == '-' && p[1] == '-' && p[2] == '>') {
            return p + 2;
        }
    }
    return NULL;
}

static bool tag_is(const char *tag, size_t len, const char *name)
{
    return strlen(name) == len && strncasecmp(tag, name, len) == 0;
}

static double *attr_slot(attrs_t *a, const char *name, size_t len)
{
    static const struct {
        const char *name;
        size_t off;
    } map[] = {
        {"Duration", offsetof(attrs_t, duration)},
        {"Power", offsetof(attrs_t, power)},
        {"PowerLow", offsetof(attrs_t, power_low)},
        {"PowerHigh", offsetof(attrs_t, power_high)},
        {"Cadence", offsetof(attrs_t, cadence)},
        {"CadenceResting", offsetof(attrs_t, cadence_resting)},
        {"Repeat", offsetof(attrs_t, repeat)},
        {"OnDuration", offsetof(attrs_t, on_duration)},
        {"OffDuration", offsetof(attrs_t, off_duration)},
        {"OnPower", offsetof(attrs_t, on_power)},
        {"OffPower", offsetof(attrs_t, off_power)},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (tag_is(name, len, map[i].name)) {
            return (double *)((char *)a + map[i].off);
        }
    }
    return NULL;
}

// Parse attributes from p up to '>'. Returns pointer after '>' or NULL.
static const char *parse_attrs(const char *p, const char *end, attrs_t *a)
{
    while (p < end && *p != '>') {
        if (!isalpha((unsigned char)*p)) {
            p++;
            continue;
        }
        const char *name = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        size_t name_len = (size_t)(p - name);
        while (p < end && (*p == ' ' || *p == '=')) {
            p++;
        }
        if (p >= end || (*p != '"' && *p != '\'')) {
            continue;
        }
        char quote = *p++;
        const char *val = p;
        while (p < end && *p != quote) {
            p++;
        }
        double *slot = attr_slot(a, name, name_len);
        if (slot && p < end) {
            char buf[32];
            size_t n = (size_t)(p - val) < sizeof(buf) - 1 ? (size_t)(p - val) : sizeof(buf) - 1;
            memcpy(buf, val, n);
            buf[n] = 0;
            *slot = strtod(buf, NULL);
        }
        p++;
    }
    return p < end ? p + 1 : NULL;
}

static uint16_t clamp_u16(double v, double max)
{
    return v <= 0 ? 0 : v >= max ? (uint16_t)max : (uint16_t)(v + 0.5);
}

static void add(workout_t *w, double dur, double p0, double p1, double cad, seg_kind_t kind)
{
    uint16_t d = clamp_u16(dur, MAX_DUR_S);
    if (d == 0 || w->n >= WORKOUT_MAX_SEGMENTS) {
        return;
    }
    segment_t *s = &w->seg[w->n++];
    s->dur_s = d;
    s->p_start = kind == SEG_FREE ? 0 : clamp_u16(p0 * 1000, MAX_POWER_PM);
    s->p_end = kind == SEG_FREE ? 0 : clamp_u16(p1 * 1000, MAX_POWER_PM);
    s->cadence = (uint8_t)clamp_u16(cad, 200);
    s->kind = (uint8_t)kind;
    w->total_s += d;
}

static void add_element(workout_t *w, const char *tag, size_t len, const attrs_t *a)
{
    if (tag_is(tag, len, "SteadyState")) {
        add(w, a->duration, a->power, a->power, a->cadence, SEG_ERG);
    } else if (tag_is(tag, len, "Warmup") || tag_is(tag, len, "Cooldown") || tag_is(tag, len, "Ramp")) {
        double lo = a->power_low, hi = a->power_high;
        if (lo == 0 && hi == 0) {
            lo = hi = a->power;
        }
        add(w, a->duration, lo, hi, a->cadence, SEG_ERG);
    } else if (tag_is(tag, len, "IntervalsT")) {
        int reps = a->repeat < 1 ? 1 : a->repeat > 100 ? 100 : (int)a->repeat;
        for (int i = 0; i < reps; i++) {
            add(w, a->on_duration, a->on_power, a->on_power, a->cadence, SEG_ERG);
            add(w, a->off_duration, a->off_power, a->off_power, a->cadence_resting, SEG_ERG);
        }
    } else if (tag_is(tag, len, "FreeRide") || tag_is(tag, len, "MaxEffort")) {
        add(w, a->duration, 0, 0, a->cadence, SEG_FREE);
    }
}

bool zwo_parse(const char *xml, size_t len, workout_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = xml, *end = xml + len;

    while (p && p < end) {
        p = memchr(p, '<', (size_t)(end - p));
        if (!p) {
            break;
        }
        p++;
        if (p < end && (*p == '!' || *p == '?' || *p == '/')) {
            // comment, declaration or closing tag
            const char *close = (*p == '!' && end - p > 3 && p[1] == '-' && p[2] == '-')
                                    ? find_comment_end(p, end)
                                    : memchr(p, '>', (size_t)(end - p));
            p = close ? close + 1 : NULL;
            continue;
        }
        const char *tag = p;
        while (p < end && (isalnum((unsigned char)*p) || *p == '_')) {
            p++;
        }
        size_t tag_len = (size_t)(p - tag);

        if (tag_is(tag, tag_len, "name")) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            if (!gt) {
                break;
            }
            const char *text = gt + 1;
            const char *lt = memchr(text, '<', (size_t)(end - text));
            size_t n = lt ? (size_t)(lt - text) : 0;
            if (n >= WORKOUT_NAME_LEN) {
                n = WORKOUT_NAME_LEN - 1;
            }
            memcpy(out->name, text, n);
            out->name[n] = 0;
            p = text;
            continue;
        }
        attrs_t a = {0};
        p = parse_attrs(p, end, &a);
        if (!p) {
            break; // truncated element: ignore it
        }
        add_element(out, tag, tag_len, &a);
    }
    return out->n > 0;
}
