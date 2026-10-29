// Zwift Accessory Protocol, plain variant (Zwift Ride, Click v2). Pure C.
// Button bits, polarity and the 0x23 message are verified against the user's
// Zwift Ride (test/host/fixtures/zap_ride_2026-10-29.txt); Click v2 is not.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZAP_COMPANY_ID 0x094A  // Zwift, in advertising manufacturer data
#define ZAP_HANDSHAKE "RideOn"
#define ZAP_HANDSHAKE_LEN 6

#define ZAP_MSG_RIDE_KEYPAD 0x23  // Async: protobuf, field 1 = button map

// Reply on SyncTX starts with "RideOn"
bool zap_is_handshake_reply(const uint8_t *d, size_t len);

// Parse an Async notification of type 0x23. Returns false for other
// messages or malformed protobuf.
bool zap_parse_ride_keypad(const uint8_t *d, size_t len, uint32_t *button_map);

// Buttons currently pressed, as a mask of ZAP_RIDE_* bits. The Ride reports
// buttons active-low (bit cleared = pressed).
uint32_t zap_ride_pressed(uint32_t button_map);

// input_id_t for one ZAP_RIDE_* bit, -1 if the button isn't used
int zap_ride_input(uint32_t bit);

// Button bits, measured on the user's Ride (2026-10-29). The community table
// (SHIFTR, qdomyos-zwift) was one bit off from Z upwards. The paddles are not
// bits but analog values (field 3), not used yet. Bits 10, 11 and 14+ were not
// pressed in the capture and are ignored.
#define ZAP_RIDE_LEFT 0x0001          // left controller
#define ZAP_RIDE_UP 0x0002
#define ZAP_RIDE_RIGHT 0x0004
#define ZAP_RIDE_DOWN 0x0008
#define ZAP_RIDE_A 0x0010             // right controller
#define ZAP_RIDE_B 0x0020
#define ZAP_RIDE_Y 0x0040
#define ZAP_RIDE_Z 0x0080
#define ZAP_RIDE_SHIFT_UP_L 0x0100    // left, upper button on the back
#define ZAP_RIDE_SHIFT_DOWN_L 0x0200  // left, lower button on the back
#define ZAP_RIDE_SHIFT_UP_R 0x1000    // right, upper button on the back
#define ZAP_RIDE_SHIFT_DOWN_R 0x2000  // right, lower button on the back
#define ZAP_RIDE_ALL 0x33FF
