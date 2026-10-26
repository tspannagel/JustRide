// Packets are hand-built from the FTMS spec. Replace/extend with real KICKR
// captures in fixtures/ once they exist.
#include "proto_ftms.h"
#include "test.h"

static void test_speed_cadence_power(void)
{
    // flags 0x0044: speed, cadence, power. 26.00 km/h, 90 rpm, 200 W
    const uint8_t pkt[] = {0x44, 0x00, 0x28, 0x0A, 0xB4, 0x00, 0xC8, 0x00};
    ftms_bike_data_t d;
    CHECK(ftms_parse_bike_data(pkt, sizeof(pkt), &d));
    CHECK_EQ(d.has, FTMS_HAS_SPEED | FTMS_HAS_CADENCE | FTMS_HAS_POWER);
    CHECK_EQ(d.speed_kmh100, 2600);
    CHECK_EQ(d.cadence_rpm10, 900);
    CHECK_EQ(d.power_w, 200);
}

static void test_more_data_has_no_speed(void)
{
    // flags 0x0041: more data (no speed), power -5 W
    const uint8_t pkt[] = {0x41, 0x00, 0xFB, 0xFF};
    ftms_bike_data_t d;
    CHECK(ftms_parse_bike_data(pkt, sizeof(pkt), &d));
    CHECK_EQ(d.has, FTMS_HAS_POWER);
    CHECK_EQ(d.power_w, -5);
}

static void test_all_fields(void)
{
    // flags 0x1FFE: everything except more-data
    const uint8_t pkt[] = {
        0xFE, 0x1F,
        0xE8, 0x03,             // speed 10.00 km/h
        0x00, 0x00,             // avg speed
        0x64, 0x00,             // cadence 50 rpm
        0x00, 0x00,             // avg cadence
        0x10, 0x27, 0x00,       // distance 10000 m
        0x0A, 0x00,             // resistance 10
        0x2C, 0x01,             // power 300 W
        0x00, 0x00,             // avg power
        0x7B, 0x00, 0, 0, 0,    // energy 123 kcal, per hour, per minute
        0x96,                   // HR 150
        0x00,                   // MET
        0x58, 0x02,             // elapsed 600 s
        0x00, 0x00,             // remaining
    };
    ftms_bike_data_t d;
    CHECK(ftms_parse_bike_data(pkt, sizeof(pkt), &d));
    CHECK_EQ(d.speed_kmh100, 1000);
    CHECK_EQ(d.cadence_rpm10, 500);
    CHECK_EQ(d.distance_m, 10000);
    CHECK_EQ(d.resistance, 10);
    CHECK_EQ(d.power_w, 300);
    CHECK_EQ(d.energy_kcal, 123);
    CHECK_EQ(d.hr_bpm, 150);
    CHECK_EQ(d.elapsed_s, 600);

    // Every truncation must be rejected
    for (size_t len = 0; len < sizeof(pkt); len++) {
        CHECK(!ftms_parse_bike_data(pkt, len, &d));
    }
}

static void test_control_point(void)
{
    uint8_t b[FTMS_CP_MAX_LEN];
    CHECK_EQ(ftms_cp_request_control(b), 1);
    CHECK_EQ(b[0], 0x00);

    CHECK_EQ(ftms_cp_target_power(b, 250), 3);
    CHECK_BYTES(b, ((uint8_t[]){0x05, 0xFA, 0x00}), 3);

    CHECK_EQ(ftms_cp_stop(b, true), 2);
    CHECK_BYTES(b, ((uint8_t[]){0x08, 0x02}), 2);

    // wind 0, grade -2.50 %, Crr 0.0040, Cw 0.51
    CHECK_EQ(ftms_cp_sim(b, 0, -250, 40, 51), 7);
    CHECK_BYTES(b, ((uint8_t[]){0x11, 0x00, 0x00, 0x06, 0xFF, 0x28, 0x33}), 7);

    uint8_t op, res;
    CHECK(ftms_parse_cp_response((uint8_t[]){0x80, 0x05, 0x01}, 3, &op, &res));
    CHECK_EQ(op, FTMS_OP_TARGET_POWER);
    CHECK_EQ(res, FTMS_RESULT_SUCCESS);
    CHECK(!ftms_parse_cp_response((uint8_t[]){0x05, 0x01}, 2, &op, &res));
}

int main(void)
{
    test_speed_cadence_power();
    test_more_data_has_no_speed();
    test_all_fields();
    test_control_point();
    return test_summary("proto_ftms");
}
