#include "input_map.h"

#include "ride.h"

int input_map(input_map_t *m, const input_evt_t *e, uint32_t now_ms, uint8_t state)
{
    if (e->id >= INPUT_IDS) {
        return -1;
    }
    bool riding = state == RIDE_RUNNING || state == RIDE_PAUSED;

    if (e->kind == IN_PRESS) {
        m->down_ms[e->id] = now_ms ? now_ms : 1;
        // Shifting reacts on press; everything else on release
        if (state == RIDE_RUNNING) {
            switch (e->id) {
            case IN_SHIFT_UP: return ACT_PLUS;
            case IN_SHIFT_DOWN: return ACT_MINUS;
            case IN_UP: return ACT_GRADE_PLUS;
            case IN_DOWN: return ACT_GRADE_MINUS;
            default: break;
            }
        }
        return -1;
    }
    if (e->kind != IN_RELEASE || m->down_ms[e->id] == 0) {
        return -1;
    }
    bool held = now_ms - m->down_ms[e->id] >= INPUT_LONG_MS;
    m->down_ms[e->id] = 0;

    switch (e->id) {
    case IN_A:
        if (riding && !held) {
            return ACT_PAUSE_TOGGLE;
        }
        if (state == RIDE_DONE && !held) {
            return ACT_DISMISS;
        }
        if (state == RIDE_IDLE && held) {
            return ACT_START_FREE;
        }
        break;
    case IN_B:
        return riding && held ? ACT_STOP : -1;
    case IN_Y:
        return state == RIDE_RUNNING && !held ? ACT_LAP : -1;
    case IN_RIGHT:
        return state == RIDE_RUNNING && held ? ACT_SKIP : -1;
    default:
        break;
    }
    return -1;
}
