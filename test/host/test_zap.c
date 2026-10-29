// Keypad decoding against packets captured from the user's Zwift Ride
// (fixtures/zap_ride_2026-10-29.txt), plus hand-built edge cases.
#include <stdlib.h>
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
    CHECK_EQ(zap_ride_input(0x0400), -1);  // not a known button

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

// Each fixture line: label, hex. Every captured press must decode to exactly its button.
static void test_ride_capture(void)
{
    static const struct {
        const char *label;
        uint32_t pressed;
        int input;
    } want[] = {
        {"idle", 0, -1},
        {"L_up", ZAP_RIDE_UP, IN_UP},
        {"L_left", ZAP_RIDE_LEFT, IN_LEFT},
        {"L_down", ZAP_RIDE_DOWN, IN_DOWN},
        {"L_right", ZAP_RIDE_RIGHT, IN_RIGHT},
        {"L_paddle_-", 0, -1},  // paddles are analog, not buttons
        {"L_paddle_+", 0, -1},
        {"L_back_up", ZAP_RIDE_SHIFT_UP_L, IN_SHIFT_UP},
        {"L_back_down", ZAP_RIDE_SHIFT_DOWN_L, IN_SHIFT_DOWN},
        {"R_Y", ZAP_RIDE_Y, IN_Y},
        {"R_Z", ZAP_RIDE_Z, IN_Z},
        {"R_B", ZAP_RIDE_B, IN_B},
        {"R_A", ZAP_RIDE_A, IN_A},
        {"R_paddle_-", 0, -1},
        {"R_paddle_+", 0, -1},
        {"R_back_up", ZAP_RIDE_SHIFT_UP_R, IN_SHIFT_UP},
        {"R_back_down", ZAP_RIDE_SHIFT_DOWN_R, IN_SHIFT_DOWN},
    };
    FILE *f = fopen(FIXTURES_DIR "/zap_ride_2026-10-29.txt", "r");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    char line[256], label[32], hex[160];
    int seen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || sscanf(line, "%31s %159s", label, hex) != 2) {
            continue;
        }
        uint8_t pkt[80];
        size_t n = strlen(hex) / 2;
        for (size_t i = 0; i < n; i++) {
            char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
            pkt[i] = (uint8_t)strtoul(b, NULL, 16);
        }
        for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
            if (strcmp(label, want[k].label) != 0) {
                continue;
            }
            uint32_t map = 0;
            CHECK(zap_parse_ride_keypad(pkt, n, &map));
            uint32_t pressed = zap_ride_pressed(map);
            if (pressed != want[k].pressed) {
                printf("fixture %s: pressed 0x%x, expected 0x%x\n", label, pressed, want[k].pressed);
            }
            CHECK_EQ(pressed, want[k].pressed);
            if (want[k].pressed) {
                CHECK_EQ(zap_ride_input(pressed), want[k].input);
            }
            seen++;
        }
    }
    fclose(f);
    CHECK_EQ(seen, (int)(sizeof(want) / sizeof(want[0])));
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
    CHECK_EQ(press(&m, IN_A, 11000, 1200, RIDE_IDLE, &p), -1);  // idle: menus, see input_nav
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

// input_nav, then input_map, as app.c calls them
static int nav(input_map_t *m, uint8_t id, uint8_t kind, uint32_t t, uint8_t state)
{
    input_evt_t e = {.id = id, .kind = kind};
    int k = input_nav(m, &e, t, state);
    input_map(m, &e, t, state);
    return k;
}

static void test_input_nav(void)
{
    input_map_t m = {0};
    CHECK_EQ(nav(&m, IN_LEFT, IN_PRESS, 100, RIDE_IDLE), NAV_PREV);
    CHECK_EQ(nav(&m, IN_UP, IN_PRESS, 100, RIDE_IDLE), NAV_PREV);
    CHECK_EQ(nav(&m, IN_RIGHT, IN_PRESS, 100, RIDE_IDLE), NAV_NEXT);
    CHECK_EQ(nav(&m, IN_DOWN, IN_PRESS, 100, RIDE_IDLE), NAV_NEXT);
    CHECK_EQ(nav(&m, IN_DOWN, IN_RELEASE, 150, RIDE_IDLE), -1);

    // A: short = enter, held = start; B short = back, held = nothing
    nav(&m, IN_A, IN_PRESS, 1000, RIDE_IDLE);
    CHECK_EQ(nav(&m, IN_A, IN_RELEASE, 1200, RIDE_IDLE), NAV_ENTER);
    nav(&m, IN_A, IN_PRESS, 2000, RIDE_IDLE);
    CHECK_EQ(nav(&m, IN_A, IN_RELEASE, 2000 + INPUT_LONG_MS, RIDE_IDLE), NAV_HOLD);
    nav(&m, IN_B, IN_PRESS, 4000, RIDE_IDLE);
    CHECK_EQ(nav(&m, IN_B, IN_RELEASE, 4100, RIDE_IDLE), NAV_BACK);
    nav(&m, IN_B, IN_PRESS, 5000, RIDE_IDLE);
    CHECK_EQ(nav(&m, IN_B, IN_RELEASE, 6500, RIDE_IDLE), -1);

    // Never during or after a ride
    CHECK_EQ(nav(&m, IN_RIGHT, IN_PRESS, 7000, RIDE_RUNNING), -1);
    nav(&m, IN_A, IN_PRESS, 8000, RIDE_PAUSED);
    CHECK_EQ(nav(&m, IN_A, IN_RELEASE, 9500, RIDE_PAUSED), -1);
    CHECK_EQ(nav(&m, IN_LEFT, IN_PRESS, 10000, RIDE_DONE), -1);

    // A release without a press (reconnect) is not an enter
    CHECK_EQ(nav(&m, IN_A, IN_RELEASE, 11000, RIDE_IDLE), -1);
}

int main(void)
{
    test_handshake();
    test_keypad();
    test_ride_capture();
    test_input_map();
    test_input_nav();
    return test_summary("zap");
}
