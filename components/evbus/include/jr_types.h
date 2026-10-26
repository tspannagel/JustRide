// Shared types: bus events, trainer commands, ride samples.
// Pure C, integers with fixed units, no floats. Keep these stable.
#pragma once

#include <stdint.h>

typedef enum {
    SRC_NONE = 0,
    SRC_TRAINER,
    SRC_HR,
    SRC_CONTROLLER,  // Zwift Ride
} source_t;

typedef struct {
    uint32_t t_ms;
    int16_t power_w;        // -1 unknown
    int16_t cadence_rpm10;  // -1 unknown
    int32_t speed_kmh100;   // -1 unknown
    int16_t hr_bpm;         // -1 unknown
    uint8_t source;         // source_t
} metrics_evt_t;

typedef enum {
    IN_SHIFT_UP, IN_SHIFT_DOWN, IN_LEFT, IN_RIGHT, IN_UP, IN_DOWN,
    IN_A, IN_B, IN_Y, IN_Z, IN_STEER, IN_BRAKE,
} input_id_t;

typedef enum { IN_PRESS, IN_RELEASE, IN_LONG_PRESS, IN_REPEAT, IN_ANALOG } input_kind_t;

typedef struct {
    uint8_t id;     // input_id_t
    uint8_t kind;   // input_kind_t
    int16_t value;  // analog value, otherwise 0
    uint8_t device;
} input_evt_t;

typedef enum {
    CMD_NONE = 0,
    CMD_ERG,
    CMD_SIM,
    CMD_RESISTANCE,
    CMD_START,
    CMD_PAUSE,
    CMD_STOP,
    CMD_SPINDOWN,
} control_kind_t;

typedef struct {
    uint8_t kind;       // control_kind_t
    int16_t target_w;   // ERG
    int16_t grade;      // SIM, 0.01 %
    uint8_t crr;        // SIM, 0.0001
    uint8_t cw;         // SIM, 0.01 kg/m
    uint8_t level;      // RESISTANCE, 0.1
} control_cmd_t;

typedef enum { DEV_CONNECTED, DEV_LOST, DEV_BATTERY, DEV_ROLE } device_kind_t;

typedef struct {
    uint8_t kind;   // device_kind_t
    uint8_t role;   // source_t
    uint8_t value;  // battery %, etc.
} device_evt_t;

// Rider actions from the touch UI (and later from controllers via input_map)
typedef enum {
    ACT_START_FREE,
    ACT_START_WORKOUT,  // workout already loaded via ride_load_workout
    ACT_PAUSE_TOGGLE,
    ACT_STOP,
    ACT_LAP,
    ACT_PLUS,           // free ride: gear up, ERG: intensity +5 %
    ACT_MINUS,
    ACT_GRADE_PLUS,     // free ride base grade +0.5 %
    ACT_GRADE_MINUS,
    ACT_SKIP,           // workout: next segment
    ACT_DISMISS,        // leave the summary screen
} action_t;

typedef enum { MODE_FREE = 0, MODE_ERG = 1 } ride_mode_t;

// One recorded second
typedef struct {
    uint32_t t_s;           // timer seconds since start (pauses excluded)
    uint32_t ts_utc;        // unix time
    int16_t power_w;        // -1 unknown
    int16_t cadence_rpm10;  // -1 unknown
    int32_t speed_kmh100;   // -1 unknown
    int16_t hr_bpm;         // -1 unknown
    int16_t target_w;       // 0 = none
    int16_t grade;          // SIM grade sent, 0.01 %
    uint8_t gear;           // 1-based, 0 = none
    uint8_t interval_idx;
    uint8_t mode;           // ride_mode_t
    uint8_t lap;
} ride_sample_t;
