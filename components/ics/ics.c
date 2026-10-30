#include "ics.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LINE_MAX_BYTES 75

typedef struct {
    char *buf;
    size_t size, len;
    bool ok;
} out_t;

static void put(out_t *o, const char *s, size_t n)
{
    if (!o->ok || o->len + n >= o->size) {
        o->ok = false;
        return;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

// One content line, folded: CRLF + space before byte 75, never inside a UTF-8 character
static void line(out_t *o, const char *s)
{
    size_t n = strlen(s), col = 0;
    for (size_t i = 0; i < n;) {
        size_t ch = 1;
        while (i + ch < n && ((unsigned char)s[i + ch] & 0xC0) == 0x80) {
            ch++;
        }
        if (col + ch > LINE_MAX_BYTES) {
            put(o, "\r\n ", 3);
            col = 1;
        }
        put(o, s + i, ch);
        col += ch;
        i += ch;
    }
    put(o, "\r\n", 2);
}

// "NAME:" + text with \ ; , escaped and newlines as \n
static void text(out_t *o, const char *name, const char *value)
{
    char l[1024];
    size_t n = (size_t)snprintf(l, sizeof(l), "%s:", name);
    for (const char *c = value ? value : ""; *c && n + 3 < sizeof(l); c++) {
        if (*c == '\r') {
            continue;
        }
        if (*c == '\n') {
            l[n++] = '\\';
            l[n++] = 'n';
        } else if (*c == '\\' || *c == ';' || *c == ',') {
            l[n++] = '\\';
            l[n++] = *c;
        } else if ((unsigned char)*c >= 0x20 || *c == '\t') {
            l[n++] = *c;
        }
    }
    l[n] = 0;
    if (n + 3 >= sizeof(l)) {
        o->ok = false;  // never cut a value short silently
    }
    line(o, l);
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

static void when(out_t *o, const char *name, uint32_t t)
{
    int y, m, d;
    civil_from_days(t / 86400, &y, &m, &d);
    uint32_t s = t % 86400;
    char l[48];
    snprintf(l, sizeof(l), "%s:%04d%02d%02dT%02u%02u%02uZ", name, y, m, d, (unsigned)(s / 3600),
             (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    line(o, l);
}

size_t ics_write(char *buf, size_t size, const ics_event_t *e)
{
    out_t o = {.buf = buf, .size = size, .ok = size > 0};
    if (size > 0) {
        buf[0] = 0;
    }
    line(&o, "BEGIN:VCALENDAR");
    line(&o, "VERSION:2.0");
    line(&o, "PRODID:-//JustRide//Head unit//EN");
    line(&o, "CALSCALE:GREGORIAN");
    line(&o, "METHOD:PUBLISH");
    line(&o, "BEGIN:VEVENT");
    text(&o, "UID", e->uid);
    when(&o, "DTSTAMP", e->stamp_utc);
    when(&o, "DTSTART", e->start_utc);
    when(&o, "DTEND", e->start_utc + e->duration_s);
    text(&o, "SUMMARY", e->summary);
    text(&o, "DESCRIPTION", e->description);
    if (e->alarm_min) {
        char l[32];
        line(&o, "BEGIN:VALARM");
        line(&o, "ACTION:DISPLAY");
        text(&o, "DESCRIPTION", e->summary);
        snprintf(l, sizeof(l), "TRIGGER:-PT%uM", e->alarm_min);
        line(&o, l);
        line(&o, "END:VALARM");
    }
    line(&o, "END:VEVENT");
    line(&o, "END:VCALENDAR");
    if (!o.ok) {
        if (size > 0) {
            buf[0] = 0;
        }
        return 0;
    }
    return o.len;
}
