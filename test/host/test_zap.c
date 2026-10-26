// Hand-built packets following the community docs. Replace with captures from
// the user's Zwift Ride (test/host/fixtures/) once recorded.
#include "input_map.h"
#include "proto_zap.h"
#include "ride.h"
#include "test.h"

// 0x23, field 1 varint = map
static size_t keypad(uint8_t *buf, uint32_t map)
{
    size_t n = 0;
    buf[n++] = ZAP_MSG_RIDE_KEYPAD;
    buf[n++] = 0x08;
    do {
        buf[n++] = (uint8_t)((map & 0x7F) | (map > 0x7F ? 0x80 : 0));
        map >>= 7;
    } while (map);
    return n;
}

static void test_handshake(void)
{
    CHECK(zap_is_handshake_reply((const uint8_t *)"RideOn\x01\x02", 8));
    CHECK(zap_is_handshake_reply((const uint8_t *)"RideOn", 6));
    CHECK(!zap_is_handshake_reply((const uint8_t *)"RideO", 5));
    CHECK(!zap_is_handshake_reply((const uint8_t *)"Hello!", 6));
}

static void test_keypad(void)
{
    uint8_t buf[32];
    uint32_t map = 0;

    size_t n = keypad(buf, 0xFFFFFFFF); // idle: nothing pressed
    CHECK(zap_parse_ride_keypad(buf, n, &map));
    CHECK_EQ(zap_ride_pressed(map), 0);

    n = keypad(buf, ~(uint32_t)(ZAP_RIDE_SHIFT_UP_R | ZAP_RIDE_A));
    // trailing analog paddle submessage (field 3, length-delimited) is skipped
    buf[n++] = 0x1A;
    buf[n++] = 0x04;
    buf[n++] = 0x08; buf[n++] = 0x01; buf[n++] = 0x10; buf[n++] = 0x64;
    CHECK(zap_parse_ride_keypad(buf, n, &map));
    CHECK_EQ(zap_ride_pressed(map), ZAP_RIDE_SHIFT_UP_R | ZAP_RIDE_A);
    CHECK_EQ(zap_ride_input(ZAP_RIDE_SHIFT_UP_R), IN_SHIFT_UP);
    CHECK_EQ(zap_ride_input(ZAP_RIDE_SHIFT_DOWN_L), IN_SHIFT_DOWN);
    CHECK_EQ(zap_ride_input(ZAP_RIDE_ONOFF_R), -1);

    // Other message types and broken protobuf are rejected
    uint8_t battery[] = {0x19, 0x08, 0x5A};
    CHECK(!zap_parse_ride_keypad(battery, sizeof(battery), &map));
    uint8_t truncated[] = {0x23, 0x08, 0xFF};
    CHECK(!zap_parse_ride_keypad(truncated, sizeof(truncated), &map));
    uint8_t bad_len[] = {0x23, 0x1A, 0x7F, 0x00};
    CHECK(!zap_parse_ride_keypad(bad_len, sizeof(bad_len), &map));
    uint8_t empty[] = {0x23};
    CHECK(!zap_parse_ride_keypad(empty, sizeof(empty), &map));
}

static int press(input_map_t *m, uint8_t id, uint32_t t, uint32_t hold, uint8_t state, int *on_press)
{
    input_evt_t e = {.id = id, .kind = IN_PRESS};
    *on_press = input_map(m, &e, t, state);
    e.kind = IN_RELEASE;
    return input_map(m, &e, t + hold, state);
}

static void test_input_map(void)
{
    input_map_t m = {0};
    int p;

    CHECK_EQ(press(&m, IN_SHIFT_UP, 1000, 100, RIDE_RUNNING, &p), -1);
    CHECK_EQ(p, ACT_PLUS);
    press(&m, IN_SHIFT_DOWN, 2000, 100, RIDE_RUNNING, &p);
    CHECK_EQ(p, ACT_MINUS);
    press(&m, IN_SHIFT_UP, 3000, 100, RIDE_IDLE, &p);
    CHECK_EQ(p, -1); // no shifting outside a ride

    // Stop only on a long press of B
    CHECK_EQ(press(&m, IN_B, 4000, 200, RIDE_RUNNING, &p), -1);
    CHECK_EQ(press(&m, IN_B, 5000, 1500, RIDE_RUNNING, &p), ACT_STOP);
    CHECK_EQ(press(&m, IN_B, 7000, 1500, RIDE_IDLE, &p), -1);

    // A: short = pause/resume, long while idle = start a free ride
    CHECK_EQ(press(&m, IN_A, 9000, 100, RIDE_RUNNING, &p), ACT_PAUSE_TOGGLE);
    CHECK_EQ(press(&m, IN_A, 9500, 100, RIDE_PAUSED, &p), ACT_PAUSE_TOGGLE);
    CHECK_EQ(press(&m, IN_A, 10000, 100, RIDE_IDLE, &p), -1);
    CHECK_EQ(press(&m, IN_A, 11000, 1200, RIDE_IDLE, &p), ACT_START_FREE);
    CHECK_EQ(press(&m, IN_A, 13000, 100, RIDE_DONE, &p), ACT_DISMISS);

    CHECK_EQ(press(&m, IN_Y, 14000, 100, RIDE_RUNNING, &p), ACT_LAP);
    CHECK_EQ(press(&m, IN_RIGHT, 15000, 100, RIDE_RUNNING, &p), -1);
    CHECK_EQ(press(&m, IN_RIGHT, 16000, 1100, RIDE_RUNNING, &p), ACT_SKIP);

    // Release without a press (e.g. after reconnect) does nothing
    input_evt_t rel = {.id = IN_B, .kind = IN_RELEASE};
    CHECK_EQ(input_map(&m, &rel, 20000, RIDE_RUNNING), -1);
    input_evt_t bad = {.id = 200, .kind = IN_PRESS};
    CHECK_EQ(input_map(&m, &bad, 20000, RIDE_RUNNING), -1);
}

int main(void)
{
    test_handshake();
    test_keypad();
    test_input_map();
    return test_summary("zap");
}
