// Pure FTMS codec: Indoor Bike Data parsing and Control Point encode/decode.
// No ESP-IDF dependencies; runs in host tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fields present in a parsed Indoor Bike Data notification
#define FTMS_HAS_SPEED (1u << 0)
#define FTMS_HAS_CADENCE (1u << 1)
#define FTMS_HAS_POWER (1u << 2)
#define FTMS_HAS_HR (1u << 3)
#define FTMS_HAS_DISTANCE (1u << 4)
#define FTMS_HAS_RESISTANCE (1u << 5)
#define FTMS_HAS_ENERGY (1u << 6)
#define FTMS_HAS_ELAPSED (1u << 7)

typedef struct {
    uint16_t has;           // FTMS_HAS_* bits
    uint16_t speed_kmh100;  // 0.01 km/h
    uint16_t cadence_rpm10; // 0.1 rpm
    int16_t power_w;
    uint8_t hr_bpm;
    uint32_t distance_m;
    int16_t resistance;     // unitless, as sent
    uint16_t energy_kcal;
    uint16_t elapsed_s;
} ftms_bike_data_t;

// Parse 0x2AD2. Returns false if the packet is shorter than its flags claim.
bool ftms_parse_bike_data(const uint8_t *buf, size_t len, ftms_bike_data_t *out);

// Control Point (0x2AD9) opcodes
#define FTMS_OP_REQUEST_CONTROL 0x00
#define FTMS_OP_RESET 0x01
#define FTMS_OP_TARGET_RESISTANCE 0x04
#define FTMS_OP_TARGET_POWER 0x05
#define FTMS_OP_START 0x07
#define FTMS_OP_STOP 0x08
#define FTMS_OP_SIM 0x11
#define FTMS_OP_SPIN_DOWN 0x13
#define FTMS_OP_RESPONSE 0x80

#define FTMS_RESULT_SUCCESS 0x01

// Every encoder writes at most this many bytes.
#define FTMS_CP_MAX_LEN 7

// Encoders return the number of bytes written to buf.
size_t ftms_cp_request_control(uint8_t *buf);
size_t ftms_cp_reset(uint8_t *buf);
size_t ftms_cp_start(uint8_t *buf);
size_t ftms_cp_stop(uint8_t *buf, bool pause);
size_t ftms_cp_target_power(uint8_t *buf, int16_t watts);
size_t ftms_cp_target_resistance(uint8_t *buf, uint8_t level10); // 0.1 resolution
size_t ftms_cp_sim(uint8_t *buf, int16_t wind_mms, int16_t grade_001pct, uint8_t crr_00001, uint8_t cw_001);
size_t ftms_cp_spin_down(uint8_t *buf);

// Parse an indication 0x80 <req op> <result>. Returns false if it isn't one.
bool ftms_parse_cp_response(const uint8_t *buf, size_t len, uint8_t *req_op, uint8_t *result);
