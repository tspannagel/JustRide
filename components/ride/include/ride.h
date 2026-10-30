// Ride state machine. Pure C: takes metrics, actions and time, produces the
// desired trainer command, 1 Hz samples and beeps. No I/O.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "jr_types.h"
#include "metrics.h"
#include "vshift.h"
#include "workout.h"

typedef enum { RIDE_IDLE, RIDE_RUNNING, RIDE_PAUSED, RIDE_DONE } ride_state_t;

// Safety limits, enforced here regardless of settings or workout content
#define RIDE_FTP_MIN 50
#define RIDE_FTP_MAX 600
#define RIDE_ERG_CAP_W 1500       // absolute ERG ceiling
#define RIDE_INTENSITY_MIN 50     // %
#define RIDE_INTENSITY_MAX 150
#define RIDE_TRAINER_STALE_MS 3000
#define RIDE_HR_STALE_MS 5000

// Adaptive segments (goal_t): the ERG target moves within the segment's bounds,
// never above Max ERG. HR is slow, so it is steered in small steps.
#define ADAPT_PERIOD_S 20        // one HR step per period
#define ADAPT_STEP_W 5
#define ADAPT_BIG_STEP_W 10      // HR more than 5 bpm above the range
#define ADAPT_HR_LOST_S 60       // no HR this long: drift back to the planned power
#define ADAPT_AVG_HORIZON_S 60   // hold-average: spread a shortfall over at least this
#define ADAPT_RAISE_S 10         // hold-average: the target rises at most ADAPT_STEP_W this often
#define ADAPT_CAD_CUE_S 5        // cadence out of range this long: cue
#define ADAPT_CAD_BEEP_GAP_S 60  // at most one cadence beep per minute

// FTP ramp test (workout_t.ramp_test): the ramp ends, and the cool-down starts, when the
// rider can't hold it. Skip ends it too; intensity stays at 100 %.
#define RAMP_FAIL_CAD_RPM 60     // cadence below this
#define RAMP_FAIL_POWER_PCT 80   // or power below this % of the target
#define RAMP_FAIL_S 10           // for this many seconds in a row (no trainer data counts too)
#define RAMP_MIN_S 120           // a result needs at least this much of the ramp (metrics_ramp_ftp)

typedef struct {
    uint16_t ftp_w;
    uint16_t max_erg_w;  // user limit, further capped by RIDE_ERG_CAP_W
    uint8_t crr;         // 0.0001
    uint8_t cw;          // 0.01 kg/m
    gear_table_t gears;
} ride_config_t;

typedef struct {
    ride_config_t cfg;
    uint8_t state;           // ride_state_t
    const workout_t *wo;     // NULL in free ride
    int32_t wo_offset_s;     // workout time = timer + offset (skips)
    int last_seg;
    bool wo_done;
    uint8_t gear;
    int16_t base_grade;      // 0.01 %
    uint8_t intensity;       // %
    uint8_t lap;

    // latest sensor values
    metrics_evt_t trainer;
    uint32_t trainer_ms;
    int16_t hr;
    uint32_t hr_ms;
    uint32_t now_ms;

    // timing
    uint32_t last_tick_ms;
    uint32_t timer_ms;       // running time only
    uint32_t n_samples;
    uint32_t start_utc;
    int16_t p3[3];           // last 3 s of power for display

    // adaptive segment state (see ride_tick)
    int adapt_seg;           // segment the values below belong to, -1 none
    int16_t adapt_w;         // current target, W
    int16_t adapt_cap_w;     // hold-average: ceiling lowered while HR is too high
    uint32_t adapt_s;        // seconds ridden in the segment
    int32_t adapt_j;         // energy in the segment, J (unknown power and not pedalling count as on target)
    uint16_t adapt_hr_lost_s;
    uint8_t adapt_tick;
    uint8_t cad_out_s;
    uint16_t cad_beep_s;
    int8_t hr_cue, cad_cue;  // -1 below range, 0 in range or no goal, +1 above

    // ramp test
    uint16_t ramp_s;         // seconds ridden in the ramp
    uint8_t ramp_low_s;      // seconds in a row below cadence or power

    metrics_t m;
} ride_t;

// Live view for the UI
typedef struct {
    uint8_t state;
    uint8_t mode;            // ride_mode_t of the current command
    uint32_t timer_s;
    int16_t power_w;         // 3 s average, -1 unknown
    int16_t cadence_rpm10;
    int32_t speed_kmh100;
    int16_t hr_bpm;
    int16_t target_w;        // ERG target, 0 = none
    uint8_t cadence_target;
    int16_t grade;           // SIM grade sent, 0.01 %
    int16_t base_grade;
    uint8_t gear;            // 1-based
    uint8_t gears;
    uint8_t intensity;
    uint8_t lap;
    bool has_workout;
    int seg_idx;
    int seg_count;
    uint32_t seg_left_s;
    bool wo_done;
    // adaptive segment
    bool adaptive;
    bool hold_avg;
    uint8_t hr_lo, hr_hi, cad_lo, cad_hi;
    int8_t hr_cue, cad_cue;  // -1 below, 0 ok, +1 above
    int16_t avg_goal_w;      // hold-average: the average to hold
    int16_t seg_avg_w;       // average power so far in this segment
    // ramp test
    bool ramp_test;
    uint8_t ramp_step;       // 1-based step while in the ramp, 0 outside
    uint16_t ramp_s;         // seconds ridden in the ramp (a result needs RAMP_MIN_S)
    metrics_summary_t sum;
} ride_view_t;

void ride_init(ride_t *r, const ride_config_t *cfg);
// Start a ride; wo == NULL for free ride. wo must stay valid until the ride ends.
void ride_start(ride_t *r, const workout_t *wo, uint32_t now_ms, uint32_t utc);
void ride_on_metrics(ride_t *r, const metrics_evt_t *e, uint32_t now_ms);
void ride_action(ride_t *r, action_t a);

// Advance time. Returns true and fills *sample when a new second was recorded.
// *beeps is set to the number of beeps to play (0 = none).
bool ride_tick(ride_t *r, uint32_t now_ms, uint32_t utc, ride_sample_t *sample, uint8_t *beeps);

// The command the trainer should have right now.
void ride_command(const ride_t *r, control_cmd_t *cmd);

void ride_view(const ride_t *r, ride_view_t *v);
