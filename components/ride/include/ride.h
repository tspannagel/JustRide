// Ride state machine. Pure C: takes metrics, actions and time, produces the
// desired trainer command, 1 Hz samples and beeps. No I/O.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "jr_types.h"
#include "metrics.h"
#include "ghost.h"
#include "route.h"
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
#define RIDE_MAX_HR_MIN 100       // rider max HR (HR zones), bpm
#define RIDE_MAX_HR_MAX 230
#define RIDE_MAX_HR_DEFAULT 185

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

// Auto-pause: no pedalling (and the flywheel nearly stopped, so coasting downhill
// doesn't count) pauses the ride; pedalling again resumes it. A pause the rider
// tapped needs a tap to resume (they may be off the bike). Never during the ramp
// of a ramp test (it has its own end rule) and never on stale trainer data.
#define AUTOPAUSE_CAD_RPM10 50      // below 5 rpm
#define AUTOPAUSE_SPEED_KMH100 500  // and below 5 km/h (if the trainer reports speed)
#define AUTOPAUSE_MS 3000
#define AUTORESUME_CAD_RPM10 300    // 30 rpm
#define AUTORESUME_MS 2000

#define RIDE_BLOCK_SHOW_S 10  // a finished lap or interval is shown this long
#define RIDE_BLOCK_MIN_S 5    // shorter ones (a quick skip) aren't

// A lap or a workout interval being ridden
typedef struct {
    uint32_t s;
    uint32_t j;               // energy, J (unknown power counts as 0, like the averages)
    uint32_t hr_sum;
    uint16_t hr_n;
} ride_block_t;

typedef enum { BLOCK_NONE, BLOCK_LAP, BLOCK_INTERVAL } block_kind_t;

// A finished one, for the ride screen
typedef struct {
    uint8_t kind;             // block_kind_t
    uint8_t no;               // 1-based lap or interval number
    uint32_t s;
    int16_t avg_w;
    int16_t avg_hr;           // 0 = none
    uint32_t until_s;         // shown while timer < this
} ride_last_block_t;

typedef struct {
    uint16_t ftp_w;
    uint16_t max_erg_w;  // user limit, further capped by RIDE_ERG_CAP_W
    uint8_t max_hr;      // bpm, for HR zones (0: RIDE_MAX_HR_DEFAULT)
    uint8_t crr;         // 0.0001
    uint8_t cw;          // 0.01 kg/m
    gear_table_t gears;
} ride_config_t;

typedef struct {
    ride_config_t cfg;
    uint8_t state;           // ride_state_t
    const workout_t *wo;     // NULL in free ride
    const route_t *route;    // route ride: SIM grade from the route by distance; NULL otherwise
    bool route_done;         // past its end: flat road
    const ghost_t *ghost;    // best earlier ride on this route, NULL: none
    uint32_t route_finish_s; // ride seconds at the route's end
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

    // lap / interval feedback, time in zones
    ride_block_t lap_blk, seg_blk;
    ride_last_block_t last_blk;
    int seg_closing;         // interval (1-based) that ended this tick, closed after its last sample
    uint32_t zone_s[METRICS_N_ZONES];
    uint32_t hr_zone_s[METRICS_N_HR_ZONES];

    // auto-pause
    bool auto_paused;        // paused by auto-pause (resumes by pedalling)
    uint32_t still_since_ms; // not pedalling since, 0 = pedalling
    uint32_t pedal_since_ms; // auto-paused and pedalling since, 0 = not

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
    bool auto_paused;
    bool route;              // riding a route
    uint32_t route_dist_m;   // distance on it
    uint32_t route_len_m;
    bool route_done;
    bool race;               // racing the best earlier ride on this route
    int32_t race_gap_s;      // seconds ahead of it (negative: behind); final once route_done
    int8_t zone;             // of the shown power, -1 unknown
    uint32_t zone_s;         // seconds in that zone this ride
    int8_t hr_zone;          // of the shown HR (% of max HR), -1 unknown
    uint32_t hr_zone_s;      // seconds in that HR zone this ride
    ride_last_block_t last_blk;  // kind BLOCK_NONE when there's nothing to show
    bool has_workout;
    int seg_idx;
    int seg_count;
    uint32_t seg_left_s;
    bool wo_done;
    // adaptive segment
    bool adaptive;
    bool hold_avg;
    uint8_t hr_lo, hr_hi, cad_lo, cad_hi;  // bpm (an HR zone already turned into bpm), rpm
    int8_t hr_goal_zone;     // the HR range is zone 0..4 of the rider's max HR, -1 otherwise
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
// Start a route ride (SIM, gears work, grade from the route). route must stay valid until the ride ends.
void ride_start_route(ride_t *r, const route_t *route, uint32_t now_ms, uint32_t utc);
// Race a best earlier ride on the route (after ride_start_route; g stays valid until the ride ends)
void ride_set_ghost(ride_t *r, const ghost_t *g);
void ride_on_metrics(ride_t *r, const metrics_evt_t *e, uint32_t now_ms);
void ride_action(ride_t *r, action_t a);

// Advance time. Returns true and fills *sample when a new second was recorded.
// *beeps is set to the number of beeps to play (0 = none).
bool ride_tick(ride_t *r, uint32_t now_ms, uint32_t utc, ride_sample_t *sample, uint8_t *beeps);

// The command the trainer should have right now.
void ride_command(const ride_t *r, control_cmd_t *cmd);

void ride_view(const ride_t *r, ride_view_t *v);
