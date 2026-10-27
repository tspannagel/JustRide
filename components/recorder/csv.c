#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rec_csv.h"

// Days since 1970-01-01 <-> civil date (H. Hinnant's algorithms), no libc TZ
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe - 719468;
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

static void split_time(uint32_t t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    civil_from_days(t / 86400, y, mo, d);
    uint32_t sec = t % 86400;
    *h = (int)(sec / 3600);
    *mi = (int)(sec / 60 % 60);
    *s = (int)(sec % 60);
}

void csv_iso_utc(char *buf, uint32_t t)
{
    int y, mo, d, h, mi, s;
    split_time(t, &y, &mo, &d, &h, &mi, &s);
    snprintf(buf, 21, "%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, h, mi, s);
}

void csv_ride_id(char *buf, uint32_t t)
{
    int y, mo, d, h, mi, s;
    split_time(t, &y, &mo, &d, &h, &mi, &s);
    snprintf(buf, 16, "%04d-%02d-%02d_%02d%02d", y, mo, d, h, mi);
}

// Fixed-point value with `dec` decimals and a trailing comma; empty if unknown
static int fmt_fixed(char *buf, size_t size, long v, int dec, bool unknown)
{
    if (unknown) {
        return snprintf(buf, size, ",");
    }
    long div = dec == 2 ? 100 : 10;
    const char *sign = v < 0 ? "-" : "";
    long a = labs(v);
    return dec == 0 ? snprintf(buf, size, "%ld,", v)
                    : snprintf(buf, size, "%s%ld.%0*ld,", sign, a / div, dec, a % div);
}

size_t csv_format_row(char *buf, size_t size, const ride_sample_t *s)
{
    char iso[21];
    csv_iso_utc(iso, s->ts_utc);
    int n = snprintf(buf, size, "%lu,%s,", (unsigned long)s->t_s, iso);
    if (n < 0 || (size_t)n >= size) {
        return 0;
    }
    struct {
        long v;
        int dec;
        bool unknown;
    } f[] = {
        {s->power_w, 0, s->power_w < 0},
        {s->cadence_rpm10, 1, s->cadence_rpm10 < 0},
        {s->speed_kmh100, 2, s->speed_kmh100 < 0},
        {s->hr_bpm, 0, s->hr_bpm <= 0},
        {s->target_w, 0, false},
        {s->grade, 2, false},
        {s->gear, 0, false},
        {s->interval_idx, 0, false},
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        int k = fmt_fixed(buf + n, size - (size_t)n, f[i].v, f[i].dec, f[i].unknown);
        if (k < 0 || (size_t)(n + k) >= size) {
            return 0;
        }
        n += k;
    }
    int k = snprintf(buf + n, size - (size_t)n, "%s,%u\n", s->mode == MODE_ERG ? "erg" : "free", s->lap);
    if (k < 0 || (size_t)(n + k) >= size) {
        return 0;
    }
    return (size_t)(n + k);
}

// Next comma-separated field; returns pointer after it or NULL at end of line
static const char *field(const char *p, char *out, size_t size)
{
    size_t n = 0;
    while (*p && *p != ',' && *p != '\n' && *p != '\r') {
        if (n + 1 < size) {
            out[n++] = *p;
        }
        p++;
    }
    out[n] = 0;
    return *p == ',' ? p + 1 : NULL;
}

static long parse_fixed(const char *f, int scale, long unknown)
{
    if (!*f) {
        return unknown;
    }
    double v = strtod(f, NULL) * scale;
    if (!isfinite(v) || v < -1e6 || v > 1e6) {
        return unknown;  // garbage; casting it to long would be undefined
    }
    return (long)(v < 0 ? v - 0.5 : v + 0.5);
}

bool csv_parse_row(const char *line, ride_sample_t *s)
{
    char f[12][24];
    const char *p = line;
    int n = 0;
    while (n < 12) {
        p = field(p, f[n++], sizeof(f[0]));
        if (!p) {
            break;
        }
    }
    if (n != 12 || f[0][0] < '0' || f[0][0] > '9') {
        return false; // header, partial line or garbage
    }
    int y, mo, d, h, mi, sec;
    if (sscanf(f[1], "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &sec) != 6) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->t_s = (uint32_t)strtoul(f[0], NULL, 10);
    s->ts_utc = (uint32_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec);
    s->power_w = (int16_t)parse_fixed(f[2], 1, -1);
    s->cadence_rpm10 = (int16_t)parse_fixed(f[3], 10, -1);
    s->speed_kmh100 = (int32_t)parse_fixed(f[4], 100, -1);
    s->hr_bpm = (int16_t)parse_fixed(f[5], 1, -1);
    s->target_w = (int16_t)parse_fixed(f[6], 1, 0);
    s->grade = (int16_t)parse_fixed(f[7], 100, 0);
    s->gear = (uint8_t)parse_fixed(f[8], 1, 0);
    s->interval_idx = (uint8_t)parse_fixed(f[9], 1, 0);
    s->mode = strcmp(f[10], "erg") == 0 ? MODE_ERG : MODE_FREE;
    s->lap = (uint8_t)parse_fixed(f[11], 1, 0);
    return true;
}

size_t csv_format_summary(char *buf, size_t size, const char *ride_id, uint32_t start_utc, const char *workout,
                          uint16_t ftp_w, const metrics_summary_t *m, const char *fw, uint8_t weight_kg,
                          uint8_t max_hr)
{
    char iso[21];
    csv_iso_utc(iso, start_utc);

    // Quote the workout name; drop characters that would break the row
    char name[64];
    size_t k = 0;
    for (const char *c = workout ? workout : ""; *c && k < sizeof(name) - 1; c++) {
        if (*c != '"' && *c != '\n' && *c != '\r') {
            name[k++] = *c;
        }
    }
    name[k] = 0;

    int n = snprintf(buf, size, "%s,%s,%lu,%lu,\"%s\",%u,%d,%d,%u.%03u,%u.%u,%lu,%d,%d,%u.%u,%lu.%03lu,%s,%u,%u\n", ride_id,
                     iso, (unsigned long)m->duration_s, (unsigned long)m->moving_s, name, ftp_w, m->avg_w, m->np_w,
                     m->if_1000 / 1000, m->if_1000 % 1000, m->tss10 / 10, m->tss10 % 10, (unsigned long)m->kj,
                     m->avg_hr, m->max_hr, m->avg_cad10 / 10, m->avg_cad10 % 10,
                     (unsigned long)(m->distance_m / 1000), (unsigned long)(m->distance_m % 1000), fw, weight_kg,
                     max_hr);
    return n > 0 && (size_t)n < size ? (size_t)n : 0;
}

size_t csv_format_bests(char *buf, size_t size, const char *ride_id, const int16_t bests[METRICS_N_BESTS])
{
    size_t n = 0;
    buf[0] = 0;
    for (int i = 0; i < METRICS_N_BESTS; i++) {
        if (bests[i] < 0) {
            continue;
        }
        int k = snprintf(buf + n, size - n, "%s,%u,%d\n", ride_id, metrics_best_s[i], bests[i]);
        if (k < 0 || n + (size_t)k >= size) {
            return 0;
        }
        n += (size_t)k;
    }
    return n;
}
