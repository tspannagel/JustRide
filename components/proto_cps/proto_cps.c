#include "proto_cps.h"

#define MAX_GAP_MS 5000

void crank_update(crank_t *c, uint32_t now_ms, int16_t cadence_rpm10)
{
    uint32_t dt = c->last_ms ? now_ms - c->last_ms : 0;
    c->last_ms = now_ms ? now_ms : 1;
    if (cadence_rpm10 <= 0 || dt == 0 || dt > MAX_GAP_MS) {
        c->frac_mrev = 0;  // the next revolution starts from here
        return;
    }
    // rpm10 / 600000 revolutions per ms = rpm10 / 600 thousandths per ms
    uint64_t mrev = c->frac_mrev + (uint64_t)cadence_rpm10 * dt / 600;
    uint32_t whole = (uint32_t)(mrev / 1000);
    c->frac_mrev = (uint32_t)(mrev % 1000);
    if (whole == 0) {
        return;
    }
    c->revs = (uint16_t)(c->revs + whole);
    // The last whole revolution ended frac_mrev thousandths of a revolution ago
    uint32_t since_ms = (uint32_t)((uint64_t)c->frac_mrev * 600 / (uint32_t)cadence_rpm10);
    c->event_1024 = (uint16_t)((uint64_t)(now_ms - since_ms) * 1024 / 1000);
}

static void put16(uint8_t *b, uint16_t v)
{
    b[0] = (uint8_t)v;
    b[1] = (uint8_t)(v >> 8);
}

size_t cps_encode_measurement(uint8_t *buf, int16_t power_w, const crank_t *c)
{
    put16(buf, 0x0020);  // crank revolution data present
    put16(buf + 2, (uint16_t)(power_w < 0 ? 0 : power_w));
    put16(buf + 4, c->revs);
    put16(buf + 6, c->event_1024);
    return CPS_MEAS_LEN;
}

size_t csc_encode_measurement(uint8_t *buf, const crank_t *c)
{
    buf[0] = 0x02;  // crank revolution data present
    put16(buf + 1, c->revs);
    put16(buf + 3, c->event_1024);
    return CSC_MEAS_LEN;
}
