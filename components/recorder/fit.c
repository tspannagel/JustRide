// FIT encoder. Field numbers/types from the FIT SDK profile (Profile.xlsx).

#include <string.h>
#include "metrics.h"
#include "rec_fit.h"

#define FIT_EPOCH 631065600u  // 1989-12-31T00:00:00Z in unix time

// Base types
#define T_ENUM 0x00
#define T_U8 0x02
#define T_U16 0x84
#define T_U32 0x86
#define T_U32Z 0x8C

// Global message numbers
#define MSG_FILE_ID 0
#define MSG_SESSION 18
#define MSG_LAP 19
#define MSG_RECORD 20
#define MSG_EVENT 21
#define MSG_DEVICE_INFO 23
#define MSG_ACTIVITY 34

// Local message types used in this file
enum { L_FILE_ID, L_DEVICE_INFO, L_EVENT, L_RECORD, L_LAP, L_SESSION, L_ACTIVITY };

// Enum values
#define EVENT_TIMER 0
#define EVENT_SESSION 8
#define EVENT_LAP 9
#define EVENT_ACTIVITY 26
#define EVENT_TYPE_START 0
#define EVENT_TYPE_STOP 1
#define EVENT_TYPE_STOP_ALL 4
#define SPORT_CYCLING 2
#define SUB_SPORT_INDOOR_CYCLING 6
#define MANUFACTURER_DEVELOPMENT 255

#define PAUSE_GAP_S 2  // ts_utc gap that counts as a pause

typedef struct {
    uint8_t num, size, type;
} fdef_t;

typedef struct {
    fit_write_fn fn;  // NULL = only count bytes
    void *ctx;
    uint16_t crc;
    uint32_t size;
    bool ok;
} writer_t;

uint16_t fit_crc16(uint16_t crc, const void *data, size_t len)
{
    static const uint16_t table[16] = {0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                       0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        uint16_t tmp = table[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ tmp ^ table[p[i] & 0xF];
        tmp = table[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ tmp ^ table[(p[i] >> 4) & 0xF];
    }
    return crc;
}

static void out(writer_t *w, const void *data, size_t len)
{
    w->crc = fit_crc16(w->crc, data, len);
    w->size += (uint32_t)len;
    if (w->fn && w->ok && !w->fn(w->ctx, data, len)) {
        w->ok = false;
    }
}

static void u8(writer_t *w, uint8_t v) { out(w, &v, 1); }

static void u16(writer_t *w, uint16_t v)
{
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    out(w, b, 2);
}

static void u32(writer_t *w, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    out(w, b, 4);
}

static void define(writer_t *w, uint8_t local, uint16_t global, const fdef_t *f, uint8_t n)
{
    u8(w, 0x40 | local);
    u8(w, 0);  // reserved
    u8(w, 0);  // little endian
    u16(w, global);
    u8(w, n);
    for (int i = 0; i < n; i++) {
        out(w, &f[i], 3);
    }
}

#define DEFINE(w, local, global, fields) define(w, local, global, fields, sizeof(fields) / sizeof(fields[0]))

// Field order below must match the order values are written in the emit_* functions.
static const fdef_t F_FILE_ID[] = {{0, 1, T_ENUM}, {1, 2, T_U16}, {2, 2, T_U16}, {3, 4, T_U32Z}, {4, 4, T_U32}};
static const fdef_t F_DEVICE_INFO[] = {{253, 4, T_U32}, {0, 1, T_U8}, {2, 2, T_U16}, {5, 2, T_U16}};
static const fdef_t F_EVENT[] = {{253, 4, T_U32}, {0, 1, T_ENUM}, {1, 1, T_ENUM}};
static const fdef_t F_RECORD[] = {{253, 4, T_U32}, {7, 2, T_U16}, {3, 1, T_U8},
                                  {4, 1, T_U8},    {6, 2, T_U16}, {5, 4, T_U32}};
// timestamp, start, elapsed, timer, distance, avg hr, max hr, avg cad, avg pwr, max pwr, event, type, index
static const fdef_t F_LAP[] = {{253, 4, T_U32}, {2, 4, T_U32},   {7, 4, T_U32},  {8, 4, T_U32}, {9, 4, T_U32},
                               {15, 1, T_U8},   {16, 1, T_U8},   {17, 1, T_U8},  {19, 2, T_U16}, {20, 2, T_U16},
                               {0, 1, T_ENUM},  {1, 1, T_ENUM},  {254, 2, T_U16}};
// timestamp, start, elapsed, timer, distance, sport, sub sport, avg hr, max hr, avg cad, avg pwr, max pwr,
// first lap, num laps, NP, TSS, IF, FTP, calories, event, type, index
static const fdef_t F_SESSION[] = {{253, 4, T_U32}, {2, 4, T_U32},  {7, 4, T_U32},  {8, 4, T_U32},  {9, 4, T_U32},
                                   {5, 1, T_ENUM},  {6, 1, T_ENUM}, {16, 1, T_U8},  {17, 1, T_U8},  {18, 1, T_U8},
                                   {20, 2, T_U16},  {21, 2, T_U16}, {25, 2, T_U16}, {26, 2, T_U16}, {34, 2, T_U16},
                                   {35, 2, T_U16},  {36, 2, T_U16}, {45, 2, T_U16}, {11, 2, T_U16}, {0, 1, T_ENUM},
                                   {1, 1, T_ENUM},  {254, 2, T_U16}};
static const fdef_t F_ACTIVITY[] = {{253, 4, T_U32}, {0, 4, T_U32},  {1, 2, T_U16}, {2, 1, T_ENUM},
                                    {3, 1, T_ENUM},  {4, 1, T_ENUM}, {5, 4, T_U32}};

static uint32_t fit_time(uint32_t unix_s) { return unix_s > FIT_EPOCH ? unix_s - FIT_EPOCH : 0; }
static uint8_t u8_or_invalid(int v) { return v > 0 && v < 255 ? (uint8_t)v : 0xFF; }
static uint16_t u16_or_invalid(long v) { return v >= 0 && v < 0xFFFF ? (uint16_t)v : 0xFFFF; }

static void event(writer_t *w, uint32_t ts, uint8_t ev, uint8_t type)
{
    u8(w, L_EVENT);
    u32(w, fit_time(ts));
    u8(w, ev);
    u8(w, type);
}

// Summary for samples [from, to)
static void summarize(const ride_sample_t *s, size_t from, size_t to, uint16_t ftp, metrics_summary_t *m)
{
    metrics_t acc;
    metrics_init(&acc);
    for (size_t i = from; i < to; i++) {
        metrics_add(&acc, &s[i]);
    }
    metrics_summary(&acc, ftp, m);
}

static uint32_t elapsed_s(const ride_sample_t *s, size_t from, size_t to)
{
    return s[to - 1].ts_utc - s[from].ts_utc + 1;
}

static void emit(writer_t *w, const ride_sample_t *s, size_t n, const fit_meta_t *meta)
{
    uint32_t t0 = s[0].ts_utc, t_end = s[n - 1].ts_utc;

    DEFINE(w, L_FILE_ID, MSG_FILE_ID, F_FILE_ID);
    DEFINE(w, L_DEVICE_INFO, MSG_DEVICE_INFO, F_DEVICE_INFO);
    DEFINE(w, L_EVENT, MSG_EVENT, F_EVENT);
    DEFINE(w, L_RECORD, MSG_RECORD, F_RECORD);
    DEFINE(w, L_LAP, MSG_LAP, F_LAP);
    DEFINE(w, L_SESSION, MSG_SESSION, F_SESSION);
    DEFINE(w, L_ACTIVITY, MSG_ACTIVITY, F_ACTIVITY);

    u8(w, L_FILE_ID);
    u8(w, 4);  // activity
    u16(w, MANUFACTURER_DEVELOPMENT);
    u16(w, 1);  // product
    u32(w, 1);  // serial
    u32(w, fit_time(t0));

    u8(w, L_DEVICE_INFO);
    u32(w, fit_time(t0));
    u8(w, 0);  // device index: creator
    u16(w, MANUFACTURER_DEVELOPMENT);
    u16(w, meta->sw_version);

    event(w, t0, EVENT_TIMER, EVENT_TYPE_START);

    uint64_t dist_mm = 0;
    size_t lap_start = 0;
    uint16_t lap_idx = 0;
    for (size_t i = 0; i < n; i++) {
        const ride_sample_t *r = &s[i];
        if (i > 0 && r->ts_utc - s[i - 1].ts_utc > PAUSE_GAP_S) {
            event(w, s[i - 1].ts_utc, EVENT_TIMER, EVENT_TYPE_STOP_ALL);
            event(w, r->ts_utc, EVENT_TIMER, EVENT_TYPE_START);
        }
        if (r->speed_kmh100 > 0) {
            dist_mm += (uint64_t)r->speed_kmh100 * 100 / 36;
        }
        u8(w, L_RECORD);
        u32(w, fit_time(r->ts_utc));
        u16(w, u16_or_invalid(r->power_w));
        u8(w, u8_or_invalid(r->hr_bpm));
        u8(w, u8_or_invalid(r->cadence_rpm10 >= 0 ? (r->cadence_rpm10 + 5) / 10 : -1));
        u16(w, u16_or_invalid(r->speed_kmh100 >= 0 ? (long)r->speed_kmh100 * 100 / 36 : -1));  // m/s x1000
        u32(w, (uint32_t)(dist_mm / 10));  // cm

        bool lap_ends = i + 1 == n || s[i + 1].lap != r->lap;
        if (lap_ends) {
            metrics_summary_t m;
            summarize(s, lap_start, i + 1, meta->ftp_w, &m);
            u8(w, L_LAP);
            u32(w, fit_time(r->ts_utc));
            u32(w, fit_time(s[lap_start].ts_utc));
            u32(w, elapsed_s(s, lap_start, i + 1) * 1000);
            u32(w, (uint32_t)(i + 1 - lap_start) * 1000);
            u32(w, m.distance_m * 100);
            u8(w, u8_or_invalid(m.avg_hr));
            u8(w, u8_or_invalid(m.max_hr));
            u8(w, u8_or_invalid((m.avg_cad10 + 5) / 10));
            u16(w, (uint16_t)m.avg_w);
            u16(w, (uint16_t)m.max_w);
            u8(w, EVENT_LAP);
            u8(w, EVENT_TYPE_STOP);
            u16(w, lap_idx++);
            lap_start = i + 1;
        }
    }

    event(w, t_end, EVENT_TIMER, EVENT_TYPE_STOP_ALL);

    metrics_summary_t m;
    summarize(s, 0, n, meta->ftp_w, &m);
    u8(w, L_SESSION);
    u32(w, fit_time(t_end));
    u32(w, fit_time(t0));
    u32(w, elapsed_s(s, 0, n) * 1000);
    u32(w, (uint32_t)n * 1000);
    u32(w, m.distance_m * 100);
    u8(w, SPORT_CYCLING);
    u8(w, SUB_SPORT_INDOOR_CYCLING);
    u8(w, u8_or_invalid(m.avg_hr));
    u8(w, u8_or_invalid(m.max_hr));
    u8(w, u8_or_invalid((m.avg_cad10 + 5) / 10));
    u16(w, (uint16_t)m.avg_w);
    u16(w, (uint16_t)m.max_w);
    u16(w, 0);  // first lap index
    u16(w, lap_idx);
    u16(w, m.np_w > 0 ? (uint16_t)m.np_w : 0xFFFF);
    u16(w, meta->ftp_w ? m.tss10 : 0xFFFF);
    u16(w, meta->ftp_w ? m.if_1000 : 0xFFFF);
    u16(w, meta->ftp_w ? meta->ftp_w : 0xFFFF);
    u16(w, (uint16_t)(m.kj > 0xFFFE ? 0xFFFE : m.kj));  // kcal ~ kJ (~24 % efficiency)
    u8(w, EVENT_SESSION);
    u8(w, EVENT_TYPE_STOP);
    u16(w, 0);

    u8(w, L_ACTIVITY);
    u32(w, fit_time(t_end));
    u32(w, (uint32_t)n * 1000);
    u16(w, 1);
    u8(w, 0);  // manual
    u8(w, EVENT_ACTIVITY);
    u8(w, EVENT_TYPE_STOP);
    u32(w, fit_time(t_end));  // local time unknown: same as UTC
}

static void header(writer_t *w, uint32_t data_size)
{
    uint8_t h[14] = {14, 0x20, 0x54, 0x08, 0, 0, 0, 0, '.', 'F', 'I', 'T', 0, 0};  // protocol 2.0, profile 21.32
    h[4] = (uint8_t)data_size;
    h[5] = (uint8_t)(data_size >> 8);
    h[6] = (uint8_t)(data_size >> 16);
    h[7] = (uint8_t)(data_size >> 24);
    uint16_t crc = fit_crc16(0, h, 12);
    h[12] = (uint8_t)crc;
    h[13] = (uint8_t)(crc >> 8);
    out(w, h, sizeof(h));
}

bool fit_write_activity(fit_write_fn fn, void *ctx, const ride_sample_t *s, size_t n, const fit_meta_t *meta)
{
    if (n == 0) {
        return false;
    }
    writer_t count = {.ok = true};
    emit(&count, s, n, meta);

    writer_t w = {.fn = fn, .ctx = ctx, .ok = true};
    header(&w, count.size);
    emit(&w, s, n, meta);
    uint16_t crc = w.crc;  // file CRC covers header + data
    uint8_t c[2] = {(uint8_t)crc, (uint8_t)(crc >> 8)};
    if (w.ok && !fn(ctx, c, 2)) {
        w.ok = false;
    }
    return w.ok;
}
