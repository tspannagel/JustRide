// Controller inputs -> ride actions, by ride state. Pure C.
// Anything that ends, starts or skips needs a long press.
#pragma once

#include <stdint.h>
#include "jr_types.h"

#define INPUT_LONG_MS 1000
#define INPUT_IDS (IN_BRAKE + 1)

typedef struct {
    uint32_t down_ms[INPUT_IDS];  // press time, 0 = not held
} input_map_t;

// Returns an action_t, or -1 for none. ride_state is a ride_state_t.
int input_map(input_map_t *m, const input_evt_t *e, uint32_t now_ms, uint8_t ride_state);

// Menu navigation keys, only while no ride is loaded (RIDE_IDLE)
typedef enum {
    NAV_PREV,   // left / up: previous item
    NAV_NEXT,   // right / down: next item
    NAV_ENTER,  // short A: press the item (never starts a ride)
    NAV_BACK,   // short B: previous screen
    NAV_HOLD,   // A held: start the highlighted ride, or a free ride
} nav_key_t;

// Returns a nav_key_t or -1. Call before input_map(), which forgets the press time.
int input_nav(const input_map_t *m, const input_evt_t *e, uint32_t now_ms, uint8_t ride_state);
