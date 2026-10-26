#include "proto_zap.h"

#include <string.h>
#include "jr_types.h"

bool zap_is_handshake_reply(const uint8_t *d, size_t len)
{
    return len >= ZAP_HANDSHAKE_LEN && memcmp(d, ZAP_HANDSHAKE, ZAP_HANDSHAKE_LEN) == 0;
}

static bool varint(const uint8_t **p, const uint8_t *end, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64 && *p < end; shift += 7) {
        uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

bool zap_parse_ride_keypad(const uint8_t *d, size_t len, uint32_t *button_map)
{
    if (len < 1 || d[0] != ZAP_MSG_RIDE_KEYPAD) {
        return false;
    }
    const uint8_t *p = d + 1, *end = d + len;
    bool found = false;
    while (p < end) {
        uint64_t key, v;
        if (!varint(&p, end, &key)) {
            return false;
        }
        switch (key & 7) {
        case 0:  // varint
            if (!varint(&p, end, &v)) {
                return false;
            }
            if (key >> 3 == 1) {
                *button_map = (uint32_t)v;
                found = true;
            }
            break;
        case 1:  // fixed64
            if (end - p < 8) {
                return false;
            }
            p += 8;
            break;
        case 2:  // length-delimited (analog paddles etc.)
            if (!varint(&p, end, &v) || v > (uint64_t)(end - p)) {
                return false;
            }
            p += v;
            break;
        case 5:  // fixed32
            if (end - p < 4) {
                return false;
            }
            p += 4;
            break;
        default:
            return false;
        }
    }
    return found;
}

uint32_t zap_ride_pressed(uint32_t button_map) { return ~button_map & ZAP_RIDE_ALL; }

int zap_ride_input(uint32_t bit)
{
    switch (bit) {
    case ZAP_RIDE_LEFT: return IN_LEFT;
    case ZAP_RIDE_UP: return IN_UP;
    case ZAP_RIDE_RIGHT: return IN_RIGHT;
    case ZAP_RIDE_DOWN: return IN_DOWN;
    case ZAP_RIDE_A: return IN_A;
    case ZAP_RIDE_B: return IN_B;
    case ZAP_RIDE_Y: return IN_Y;
    case ZAP_RIDE_Z: return IN_Z;
    case ZAP_RIDE_SHIFT_UP_L:
    case ZAP_RIDE_SHIFT_UP_R: return IN_SHIFT_UP;
    case ZAP_RIDE_SHIFT_DOWN_L:
    case ZAP_RIDE_SHIFT_DOWN_R: return IN_SHIFT_DOWN;
    default: return -1;  // power-up and on/off buttons are not used
    }
}
