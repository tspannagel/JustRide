#include "proto_ftms.h"

#include <string.h>

// Indoor Bike Data flags (FTMS spec 4.9.1). Bit 0 is inverted: 0 = speed present.
#define IBD_MORE_DATA (1u << 0)
#define IBD_AVG_SPEED (1u << 1)
#define IBD_CADENCE (1u << 2)
#define IBD_AVG_CADENCE (1u << 3)
#define IBD_DISTANCE (1u << 4)
#define IBD_RESISTANCE (1u << 5)
#define IBD_POWER (1u << 6)
#define IBD_AVG_POWER (1u << 7)
#define IBD_ENERGY (1u << 8)
#define IBD_HR (1u << 9)
#define IBD_MET (1u << 10)
#define IBD_ELAPSED (1u << 11)
#define IBD_REMAINING (1u << 12)

typedef struct {
    const uint8_t *p;
    size_t left;
    bool ok;
} reader_t;

static const uint8_t *take(reader_t *r, size_t n)
{
    if (!r->ok || r->left < n) {
        r->ok = false;
        return NULL;
    }
    const uint8_t *p = r->p;
    r->p += n;
    r->left -= n;
    return p;
}

static uint8_t u8(reader_t *r)
{
    const uint8_t *p = take(r, 1);
    return p ? p[0] : 0;
}

static uint16_t u16(reader_t *r)
{
    const uint8_t *p = take(r, 2);
    return p ? (uint16_t)(p[0] | p[1] << 8) : 0;
}

static uint32_t u24(reader_t *r)
{
    const uint8_t *p = take(r, 3);
    return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 : 0;
}

bool ftms_parse_bike_data(const uint8_t *buf, size_t len, ftms_bike_data_t *out)
{
    memset(out, 0, sizeof(*out));
    reader_t r = {buf, len, true};
    uint16_t flags = u16(&r);

    if (!(flags & IBD_MORE_DATA)) {
        out->speed_kmh100 = u16(&r);
        out->has |= FTMS_HAS_SPEED;
    }
    if (flags & IBD_AVG_SPEED) {
        take(&r, 2);
    }
    if (flags & IBD_CADENCE) {
        out->cadence_rpm10 = (uint16_t)(u16(&r) * 5); // 0.5 rpm -> 0.1 rpm
        out->has |= FTMS_HAS_CADENCE;
    }
    if (flags & IBD_AVG_CADENCE) {
        take(&r, 2);
    }
    if (flags & IBD_DISTANCE) {
        out->distance_m = u24(&r);
        out->has |= FTMS_HAS_DISTANCE;
    }
    if (flags & IBD_RESISTANCE) {
        out->resistance = (int16_t)u16(&r);
        out->has |= FTMS_HAS_RESISTANCE;
    }
    if (flags & IBD_POWER) {
        out->power_w = (int16_t)u16(&r);
        out->has |= FTMS_HAS_POWER;
    }
    if (flags & IBD_AVG_POWER) {
        take(&r, 2);
    }
    if (flags & IBD_ENERGY) {
        out->energy_kcal = u16(&r);
        take(&r, 3); // per hour (u16), per minute (u8)
        out->has |= FTMS_HAS_ENERGY;
    }
    if (flags & IBD_HR) {
        out->hr_bpm = u8(&r);
        out->has |= FTMS_HAS_HR;
    }
    if (flags & IBD_MET) {
        take(&r, 1);
    }
    if (flags & IBD_ELAPSED) {
        out->elapsed_s = u16(&r);
        out->has |= FTMS_HAS_ELAPSED;
    }
    if (flags & IBD_REMAINING) {
        take(&r, 2);
    }
    return r.ok;
}

static size_t put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 2;
}

size_t ftms_cp_request_control(uint8_t *buf)
{
    buf[0] = FTMS_OP_REQUEST_CONTROL;
    return 1;
}

size_t ftms_cp_reset(uint8_t *buf)
{
    buf[0] = FTMS_OP_RESET;
    return 1;
}

size_t ftms_cp_start(uint8_t *buf)
{
    buf[0] = FTMS_OP_START;
    return 1;
}

size_t ftms_cp_stop(uint8_t *buf, bool pause)
{
    buf[0] = FTMS_OP_STOP;
    buf[1] = pause ? 0x02 : 0x01;
    return 2;
}

size_t ftms_cp_target_power(uint8_t *buf, int16_t watts)
{
    buf[0] = FTMS_OP_TARGET_POWER;
    return 1 + put16(buf + 1, (uint16_t)watts);
}

size_t ftms_cp_target_resistance(uint8_t *buf, uint8_t level10)
{
    buf[0] = FTMS_OP_TARGET_RESISTANCE;
    buf[1] = level10;
    return 2;
}

size_t ftms_cp_sim(uint8_t *buf, int16_t wind_mms, int16_t grade_001pct, uint8_t crr_00001, uint8_t cw_001)
{
    size_t n = 0;
    buf[n++] = FTMS_OP_SIM;
    n += put16(buf + n, (uint16_t)wind_mms);
    n += put16(buf + n, (uint16_t)grade_001pct);
    buf[n++] = crr_00001;
    buf[n++] = cw_001;
    return n;
}

size_t ftms_cp_spin_down(uint8_t *buf)
{
    buf[0] = FTMS_OP_SPIN_DOWN;
    buf[1] = 0x01; // start
    return 2;
}

bool ftms_parse_cp_response(const uint8_t *buf, size_t len, uint8_t *req_op, uint8_t *result)
{
    if (len < 3 || buf[0] != FTMS_OP_RESPONSE) {
        return false;
    }
    *req_op = buf[1];
    *result = buf[2];
    return true;
}
