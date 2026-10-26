// Zwift Accessory Protocol, plain variant (Zwift Ride, Click v2). Pure C.
// Community reverse-engineered, NOT verified on the user's devices yet:
// button bits, polarity and message ids must be checked against raw dumps
// (diag: `zap dump on`) and turned into fixtures in test/host/fixtures/.
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

// Button bits (from community projects: SHIFTR, qdomyos-zwift; unverified)
#define ZAP_RIDE_LEFT 0x00001
#define ZAP_RIDE_UP 0x00002
#define ZAP_RIDE_RIGHT 0x00004
#define ZAP_RIDE_DOWN 0x00008
#define ZAP_RIDE_A 0x00010
#define ZAP_RIDE_B 0x00020
#define ZAP_RIDE_Y 0x00040
#define ZAP_RIDE_Z 0x00100
#define ZAP_RIDE_SHIFT_UP_L 0x00200
#define ZAP_RIDE_SHIFT_DOWN_L 0x00400
#define ZAP_RIDE_POWERUP_L 0x00800
#define ZAP_RIDE_ONOFF_L 0x01000
#define ZAP_RIDE_SHIFT_UP_R 0x02000
#define ZAP_RIDE_SHIFT_DOWN_R 0x04000
#define ZAP_RIDE_POWERUP_R 0x10000
#define ZAP_RIDE_ONOFF_R 0x20000
#define ZAP_RIDE_ALL 0x37FFF
