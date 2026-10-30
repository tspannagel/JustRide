#include "proto_cps.h"
#include "test.h"

// Cadence a receiver computes from two measurements (rpm x10)
static int rx_cadence10(const crank_t *a, const crank_t *b)
{
    uint16_t dr = (uint16_t)(b->revs - a->revs), dt = (uint16_t)(b->event_1024 - a->event_1024);
    return dt ? (int)((uint32_t)dr * 1024 * 600 / dt) : 0;
}

static void test_steady(void)
{
    crank_t c = {0};
    uint32_t t = 100000;
    crank_update(&c, t, 900);
    CHECK_EQ(c.revs, 0);
    crank_t prev = c;
    // 90 rpm for 60 s at 1 Hz: 90 revolutions; receivers see about 90 rpm
    for (int i = 0; i < 60; i++) {
        t += 1000;
        crank_update(&c, t, 900);
        if (i > 0 && c.revs != prev.revs) {
            int rx = rx_cadence10(&prev, &c);
            CHECK(rx > 870 && rx < 930);
        }
        if (c.revs != prev.revs) {
            prev = c;
        }
    }
    CHECK_EQ(c.revs, 90);
}

static void test_slow_and_stop(void)
{
    crank_t c = {0};
    uint32_t t = 5000;
    crank_update(&c, t, 300);  // 30 rpm: one revolution every 2 s
    t += 1000;
    crank_update(&c, t, 300);
    CHECK_EQ(c.revs, 0);
    t += 1000;
    crank_update(&c, t, 300);
    CHECK_EQ(c.revs, 1);
    CHECK_EQ(c.event_1024, (uint16_t)((uint64_t)t * 1024 / 1000));
    uint16_t ev = c.event_1024;
    // Stopped: nothing changes, so receivers fall back to 0 rpm
    for (int i = 0; i < 5; i++) {
        t += 1000;
        crank_update(&c, t, 0);
    }
    CHECK_EQ(c.revs, 1);
    CHECK_EQ(c.event_1024, ev);
    // Unknown cadence (-1) and long gaps add nothing
    crank_update(&c, t + 1000, -1);
    crank_update(&c, t + 60000, 900);
    CHECK_EQ(c.revs, 1);
}

static void test_wrap(void)
{
    crank_t c = {.revs = 65535, .last_ms = 1000};
    crank_update(&c, 2000, 1200);  // 2 revolutions in 1 s at 120 rpm
    CHECK_EQ(c.revs, 1);
    // 1/1024 s timestamps wrap after 64 s
    crank_t d = {.last_ms = 70000};
    crank_update(&d, 71000, 600);
    CHECK_EQ(d.event_1024, (uint16_t)(71000ull * 1024 / 1000));
}

static void test_encode(void)
{
    crank_t c = {.revs = 0x1234, .event_1024 = 0xABCD};
    uint8_t b[CPS_MEAS_LEN];
    CHECK_EQ(cps_encode_measurement(b, 250, &c), 8);
    const uint8_t want[] = {0x20, 0x00, 0xFA, 0x00, 0x34, 0x12, 0xCD, 0xAB};
    CHECK_BYTES(b, want, 8);
    cps_encode_measurement(b, -1, &c);
    CHECK_EQ(b[2] | b[3] << 8, 0);  // unknown power is sent as 0

    uint8_t s[CSC_MEAS_LEN];
    CHECK_EQ(csc_encode_measurement(s, &c), 5);
    const uint8_t want_csc[] = {0x02, 0x34, 0x12, 0xCD, 0xAB};
    CHECK_BYTES(s, want_csc, 5);
}

int main(void)
{
    test_steady();
    test_slow_and_stop();
    test_wrap();
    test_encode();
    return test_summary("cps");
}
