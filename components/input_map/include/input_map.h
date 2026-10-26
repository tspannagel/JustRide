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
