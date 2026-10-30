#include "ride.h"

#include <string.h>

#define GRADE_STEP 50  // 0.5 %
#define INTENSITY_STEP 5

static uint16_t clamp_u16(int v, int lo, int hi) { return (uint16_t)(v < lo ? lo : v > hi ? hi : v); }

void ride_init(ride_t *r, const ride_config_t *cfg)
{
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    r->cfg.ftp_w = clamp_u16(cfg->ftp_w, RIDE_FTP_MIN, RIDE_FTP_MAX);
    r->cfg.max_erg_w = clamp_u16(cfg->max_erg_w, 0, RIDE_ERG_CAP_W);
    if (r->cfg.gears.n == 0) {
        vshift_default(&r->cfg.gears);
    }
    r->state = RIDE_IDLE;
    r->hr = -1;
    r->trainer.power_w = r->trainer.cadence_rpm10 = r->trainer.hr_bpm = -1;
    r->trainer.speed_kmh100 = -1;
    r->gear = (uint8_t)vshift_neutral_gear(&r->cfg.gears);
    r->intensity = 100;
    r->adapt_seg = -1;
    r->cad_beep_s = ADAPT_CAD_BEEP_GAP_S;
}

void ride_start(ride_t *r, const workout_t *wo, uint32_t now_ms, uint32_t utc)
{
    ride_config_t cfg = r->cfg;
    metrics_evt_t trainer = r->trainer;
    uint32_t trainer_ms = r->trainer_ms;
    int16_t hr = r->hr;
    uint32_t hr_ms = r->hr_ms;

    ride_init(r, &cfg);
    // keep live sensor values across the reset
    r->trainer = trainer;
    r->trainer_ms = trainer_ms;
    r->hr = hr;
    r->hr_ms = hr_ms;

    r->wo = wo && wo->n > 0 ? wo : NULL;
    r->last_seg = -1;
    r->state = RIDE_RUNNING;
    r->now_ms = r->last_tick_ms = now_ms;
    r->start_utc = utc;
    r->p3[0] = r->p3[1] = r->p3[2] = -1;
    metrics_init(&r->m);
}

void ride_on_metrics(ride_t *r, const metrics_evt_t *e, uint32_t now_ms)
{
    if (e->source == SRC_HR) {
        if (e->hr_bpm > 0) {
            r->hr = e->hr_bpm;
            r->hr_ms = now_ms;
        }
        return;
    }
    // Trainer: notifications may carry only some fields (FTMS "more data")
    if (e->power_w >= 0) {
        r->trainer.power_w = e->power_w;
    }
    if (e->cadence_rpm10 >= 0) {
        r->trainer.cadence_rpm10 = e->cadence_rpm10;
    }
    if (e->speed_kmh100 >= 0) {
        r->trainer.speed_kmh100 = e->speed_kmh100;
    }
    if (e->hr_bpm > 0) {
        r->trainer.hr_bpm = e->hr_bpm;
    }
    r->trainer_ms = now_ms;
}

// Signed: data that arrived after the last tick is newer than r->now_ms, not 49 days old
static bool fresh_since(const ride_t *r, uint32_t t_ms, uint32_t max_ms)
{
    return t_ms != 0 && (int32_t)(r->now_ms - t_ms) <= (int32_t)max_ms;
}

static bool trainer_fresh(const ride_t *r) { return fresh_since(r, r->trainer_ms, RIDE_TRAINER_STALE_MS); }

static int16_t current_hr(const ride_t *r)
{
    if (fresh_since(r, r->hr_ms, RIDE_HR_STALE_MS)) {
        return r->hr;
    }
    return trainer_fresh(r) && r->trainer.hr_bpm > 0 ? r->trainer.hr_bpm : -1;
}

static uint32_t wo_time(const ride_t *r)
{
    int32_t t = (int32_t)(r->timer_ms / 1000) + r->wo_offset_s;
    return t < 0 ? 0 : (uint32_t)t;
}

static bool in_erg(const ride_t *r, workout_pos_t *pos)
{
    if (!r->wo || r->wo_done) {
        return false;
    }
    workout_at(r->wo, wo_time(r), pos);
    return !pos->done && pos->kind == SEG_ERG;
}

static int16_t watts(const ride_t *r, uint16_t pm)
{
    return (int16_t)((uint32_t)pm * r->cfg.ftp_w / 1000 * r->intensity / 100);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// Bounds of an adaptive segment in W, never above Max ERG
static void adapt_bounds(const ride_t *r, const goal_t *g, int *lo, int *hi)
{
    *hi = watts(r, g->max_pm) < r->cfg.max_erg_w ? watts(r, g->max_pm) : r->cfg.max_erg_w;
    *lo = watts(r, g->min_pm) < *hi ? watts(r, g->min_pm) : *hi;
}

// Once per recorded second while running. HR above the range always wins: it
// lowers the target (hold-average: a ceiling); below the range it raises it only
// in segments without an average to hold; without HR it never raises it.
// Hold-average spreads any shortfall over the rest of the segment, but seconds
// without pedalling (a stop to drink) are no shortfall, and the target rises
// slowly, so the rider never comes back to a sudden jump.
static void adapt_update(ride_t *r, uint8_t *beeps)
{
    workout_pos_t pos;
    const goal_t *g = in_erg(r, &pos) ? workout_goal(r->wo, pos.idx) : NULL;
    r->hr_cue = r->cad_cue = 0;
    if (r->cad_beep_s < UINT16_MAX) {
        r->cad_beep_s++;
    }
    if (!g) {
        r->adapt_seg = -1;
        return;
    }
    int base = watts(r, pos.target_pm), lo, hi;
    adapt_bounds(r, g, &lo, &hi);
    if (pos.idx != r->adapt_seg) {
        r->adapt_seg = pos.idx;
        r->adapt_w = (int16_t)clampi(base, lo, hi);
        r->adapt_cap_w = (int16_t)hi;
        r->adapt_s = 0;
        r->adapt_j = 0;
        r->adapt_hr_lost_s = 0;
        r->adapt_tick = 0;
        r->cad_out_s = 0;
    }
    bool fresh = trainer_fresh(r);
    int power = fresh ? r->trainer.power_w : -1;
    int cad10 = fresh ? r->trainer.cadence_rpm10 : -1;
    r->adapt_s++;
    r->adapt_j += power < 0 || cad10 == 0 ? r->adapt_w : power;

    int hr = current_hr(r);
    if (g->hr_hi) {
        r->adapt_hr_lost_s = hr < 0 ? (uint16_t)(r->adapt_hr_lost_s + 1) : 0;
        r->hr_cue = (int8_t)(hr < 0 ? 0 : hr > g->hr_hi ? 1 : hr < g->hr_lo ? -1 : 0);
        if (++r->adapt_tick >= ADAPT_PERIOD_S) {
            r->adapt_tick = 0;
            int step = hr > g->hr_hi + 5 ? ADAPT_BIG_STEP_W : ADAPT_STEP_W;
            if (g->hold_avg) {
                if (r->hr_cue > 0) {
                    r->adapt_cap_w = (int16_t)((r->adapt_w < r->adapt_cap_w ? r->adapt_w : r->adapt_cap_w) - step);
                } else if (hr >= 0) {
                    r->adapt_cap_w = (int16_t)(r->adapt_cap_w + ADAPT_STEP_W);  // let the average recover
                }
                r->adapt_cap_w = (int16_t)clampi(r->adapt_cap_w, lo, hi);
            } else if (r->hr_cue > 0) {
                r->adapt_w = (int16_t)(r->adapt_w - step);
            } else if (r->hr_cue < 0) {
                r->adapt_w = (int16_t)(r->adapt_w + ADAPT_STEP_W);
            } else if (hr < 0 && r->adapt_hr_lost_s >= ADAPT_HR_LOST_S && r->adapt_w > base) {
                r->adapt_w = (int16_t)(r->adapt_w - ADAPT_STEP_W);  // no HR: back towards the plan, never up
            }
        }
    }
    if (g->hold_avg) {
        int32_t left = (int32_t)pos.seg_left_s > ADAPT_AVG_HORIZON_S ? (int32_t)pos.seg_left_s : ADAPT_AVG_HORIZON_S;
        int32_t want = base + ((int32_t)base * (int32_t)r->adapt_s - r->adapt_j) / left;
        int32_t rise = r->adapt_s % ADAPT_RAISE_S == 0 ? ADAPT_STEP_W : 0;
        want = want > r->adapt_w + rise ? r->adapt_w + rise : want;
        r->adapt_w = (int16_t)clampi(want, lo, g->hr_hi ? r->adapt_cap_w : hi);
    }
    r->adapt_w = (int16_t)clampi(r->adapt_w, lo, hi);

    // Cadence: a cue only, and only while pedalling
    if (g->cad_hi && cad10 > 0) {
        int rpm = cad10 / 10, out = rpm < g->cad_lo ? -1 : rpm > g->cad_hi ? 1 : 0;
        r->cad_out_s = out ? (uint8_t)(r->cad_out_s < 255 ? r->cad_out_s + 1 : 255) : 0;
        if (r->cad_out_s >= ADAPT_CAD_CUE_S) {
            r->cad_cue = (int8_t)out;
            if (r->cad_out_s == ADAPT_CAD_CUE_S && r->cad_beep_s >= ADAPT_CAD_BEEP_GAP_S && *beeps == 0) {
                *beeps = 1;
                r->cad_beep_s = 0;
            }
        }
    } else {
        r->cad_out_s = 0;
    }
}

// Segment index while in the ramp of a ramp test, -1 otherwise
static int ramp_idx(const ride_t *r)
{
    workout_pos_t pos;
    if (!r->wo || !r->wo->ramp_test || !in_erg(r, &pos)) {
        return -1;
    }
    return pos.idx >= r->wo->ramp_first && pos.idx < r->wo->ramp_end ? pos.idx : -1;
}

// On to the cool-down
static void ramp_end(ride_t *r)
{
    r->seg_closing = r->last_seg + 1;
    r->wo_offset_s += (int32_t)workout_seg_start(r->wo, r->wo->ramp_end) - (int32_t)wo_time(r);
    r->last_seg = r->wo->ramp_end;  // no segment beeps on top of the end beeps
}

// Once per recorded second: count the ramp and end it when the rider can't hold it.
// Stale trainer data counts as failing: the ramp keeps climbing while the trainer is
// gone, and a rider must not come back to a step they never rode up to.
static void ramp_update(ride_t *r, const ride_sample_t *s, uint8_t *beeps)
{
    if (ramp_idx(r) < 0) {
        return;
    }
    r->ramp_s++;
    bool low = s->power_w < 0 || s->cadence_rpm10 < 0 || s->cadence_rpm10 < RAMP_FAIL_CAD_RPM * 10 ||
               s->power_w * 100 < s->target_w * RAMP_FAIL_POWER_PCT;
    r->ramp_low_s = low ? (uint8_t)(r->ramp_low_s + 1) : 0;
    if (r->ramp_low_s >= RAMP_FAIL_S) {
        ramp_end(r);
        *beeps = 3;
    }
}

static bool gear_mode(const ride_t *r)
{
    workout_pos_t pos;
    return !in_erg(r, &pos);
}

static void block_add(ride_block_t *b, const ride_sample_t *s)
{
    b->s++;
    b->j += s->power_w > 0 ? (uint32_t)s->power_w : 0;
    if (s->hr_bpm > 0) {
        b->hr_sum += (uint32_t)s->hr_bpm;
        b->hr_n++;
    }
}

// A lap or interval ended: show it for a while (unless it was too short), start the next
static void block_close(ride_t *r, ride_block_t *b, uint8_t kind, int no)
{
    if (b->s >= RIDE_BLOCK_MIN_S) {
        r->last_blk = (ride_last_block_t){
            .kind = kind,
            .no = (uint8_t)(no > 255 ? 255 : no),
            .s = b->s,
            .avg_w = (int16_t)(b->j / b->s),
            .avg_hr = (int16_t)(b->hr_n ? b->hr_sum / b->hr_n : 0),
            .until_s = r->timer_ms / 1000 + RIDE_BLOCK_SHOW_S,
        };
    }
    memset(b, 0, sizeof(*b));
}

// Every tick, running or paused (see AUTOPAUSE_*)
static void auto_pause(ride_t *r)
{
    bool fresh = trainer_fresh(r);
    int cad = r->trainer.cadence_rpm10, spd = r->trainer.speed_kmh100;
    if (r->state == RIDE_RUNNING) {
        bool still = fresh && cad >= 0 && cad < AUTOPAUSE_CAD_RPM10 && (spd < 0 || spd < AUTOPAUSE_SPEED_KMH100);
        if (!still || ramp_idx(r) >= 0) {
            r->still_since_ms = 0;
        } else if (!r->still_since_ms) {
            r->still_since_ms = r->now_ms ? r->now_ms : 1;
        } else if (r->now_ms - r->still_since_ms >= AUTOPAUSE_MS) {
            r->state = RIDE_PAUSED;
            r->auto_paused = true;
            r->still_since_ms = r->pedal_since_ms = 0;
        }
    } else if (r->state == RIDE_PAUSED && r->auto_paused) {
        if (!fresh || cad < AUTORESUME_CAD_RPM10) {
            r->pedal_since_ms = 0;
        } else if (!r->pedal_since_ms) {
            r->pedal_since_ms = r->now_ms ? r->now_ms : 1;
        } else if (r->now_ms - r->pedal_since_ms >= AUTORESUME_MS) {
            r->state = RIDE_RUNNING;
            r->auto_paused = false;
            r->pedal_since_ms = 0;
        }
    }
}

void ride_action(ride_t *r, action_t a)
{
    switch (a) {
    case ACT_PAUSE_TOGGLE:
        if (r->state == RIDE_RUNNING) {
            r->state = RIDE_PAUSED;
        } else if (r->state == RIDE_PAUSED) {
            r->state = RIDE_RUNNING;
        }
        r->auto_paused = false;  // a tapped pause needs a tap to resume
        r->still_since_ms = 0;
        break;
    case ACT_STOP:
        if (r->state == RIDE_RUNNING || r->state == RIDE_PAUSED) {
            r->state = RIDE_DONE;
        }
        break;
    case ACT_DISMISS:
        if (r->state == RIDE_DONE) {
            r->state = RIDE_IDLE;
            r->wo = NULL;
        }
        break;
    case ACT_LAP:
        if (r->state == RIDE_RUNNING && r->lap < 255) {
            block_close(r, &r->lap_blk, BLOCK_LAP, r->lap + 1);
            r->lap++;
        }
        break;
    case ACT_PLUS:
    case ACT_MINUS: {
        int d = a == ACT_PLUS ? 1 : -1;
        if (r->wo && r->wo->ramp_test && !gear_mode(r)) {
            break;  // a test at 90 % would give a wrong FTP
        }
        if (gear_mode(r)) {
            r->gear = (uint8_t)clamp_u16(r->gear + d, 0, r->cfg.gears.n - 1);
        } else {
            r->intensity = (uint8_t)clamp_u16(r->intensity + d * INTENSITY_STEP, RIDE_INTENSITY_MIN, RIDE_INTENSITY_MAX);
        }
        break;
    }
    case ACT_GRADE_PLUS:
    case ACT_GRADE_MINUS: {
        int g = r->base_grade + (a == ACT_GRADE_PLUS ? GRADE_STEP : -GRADE_STEP);
        r->base_grade = (int16_t)(g < VSHIFT_GRADE_MIN ? VSHIFT_GRADE_MIN : g > VSHIFT_GRADE_MAX ? VSHIFT_GRADE_MAX : g);
        break;
    }
    case ACT_SKIP:
        if (r->state == RIDE_RUNNING && ramp_idx(r) >= 0) {
            ramp_end(r);  // the rider ends the test
        } else if (r->wo && !r->wo_done && r->state == RIDE_RUNNING) {
            workout_pos_t pos;
            workout_at(r->wo, wo_time(r), &pos);
            r->wo_offset_s += (int32_t)pos.seg_left_s;
        }
        break;
    default:
        break;
    }
}

void ride_command(const ride_t *r, control_cmd_t *cmd)
{
    memset(cmd, 0, sizeof(*cmd));
    if (r->state != RIDE_RUNNING) {
        // Idle, paused, finished: lowest resistance so the rider is never stuck
        cmd->kind = CMD_RESISTANCE;
        cmd->level = 0;
        return;
    }
    workout_pos_t pos;
    if (in_erg(r, &pos)) {
        uint32_t w = (uint32_t)watts(r, pos.target_pm);
        const goal_t *g = workout_goal(r->wo, pos.idx);
        if (g) {
            // Adaptive: the steered target in 5 W steps (fewer trainer writes), within the bounds
            int lo, hi;
            adapt_bounds(r, g, &lo, &hi);
            int a = r->adapt_seg == pos.idx ? (r->adapt_w + 2) / 5 * 5 : (int)w;
            w = (uint32_t)clampi(a, lo, hi);
        }
        cmd->kind = CMD_ERG;
        cmd->target_w = (int16_t)(w > r->cfg.max_erg_w ? r->cfg.max_erg_w : w);
        return;
    }
    cmd->kind = CMD_SIM;
    cmd->grade = vshift_grade(&r->cfg.gears, r->gear, r->base_grade, r->cfg.crr);
    cmd->crr = r->cfg.crr;
    cmd->cw = r->cfg.cw;
}

static void fill_sample(const ride_t *r, ride_sample_t *s, uint32_t utc)
{
    bool fresh = trainer_fresh(r);
    control_cmd_t cmd;
    ride_command(r, &cmd);
    workout_pos_t pos = {0};
    if (r->wo) {
        workout_at(r->wo, wo_time(r), &pos);
    }
    memset(s, 0, sizeof(*s));
    s->t_s = r->n_samples;
    s->ts_utc = utc;
    s->power_w = fresh ? r->trainer.power_w : -1;
    s->cadence_rpm10 = fresh ? r->trainer.cadence_rpm10 : -1;
    s->speed_kmh100 = fresh ? r->trainer.speed_kmh100 : -1;
    s->hr_bpm = current_hr(r);
    s->mode = cmd.kind == CMD_ERG ? MODE_ERG : MODE_FREE;
    s->target_w = cmd.kind == CMD_ERG ? cmd.target_w : 0;
    s->grade = cmd.kind == CMD_SIM ? cmd.grade : 0;
    s->gear = cmd.kind == CMD_SIM ? r->gear + 1 : 0;
    s->interval_idx = r->wo ? (uint8_t)pos.idx : 0;
    s->lap = r->lap;
}

bool ride_tick(ride_t *r, uint32_t now_ms, uint32_t utc, ride_sample_t *sample, uint8_t *beeps)
{
    *beeps = 0;
    uint32_t dt = now_ms - r->last_tick_ms;
    r->last_tick_ms = now_ms;
    r->now_ms = now_ms;
    auto_pause(r);
    if (r->state != RIDE_RUNNING) {
        return false;
    }
    r->timer_ms += dt;

    if (r->wo && !r->wo_done) {
        workout_pos_t pos;
        workout_at(r->wo, wo_time(r), &pos);
        if (pos.done) {
            r->wo_done = true; // continue as free ride
            *beeps = 3;
            r->seg_closing = r->last_seg + 1;
        } else if (pos.idx != r->last_seg) {
            *beeps = r->last_seg < 0 ? 0 : 2;
            if (r->last_seg >= 0) {
                r->seg_closing = r->last_seg + 1;
            }
            r->last_seg = pos.idx;
        }
    }

    // One sample per call; if a tick was late, later ticks catch up
    if (r->timer_ms / 1000 <= r->n_samples) {
        return false;
    }
    adapt_update(r, beeps);
    fill_sample(r, sample, utc);
    ramp_update(r, sample, beeps);
    // This second still belongs to the interval that just ended (sample t covers t..t+1)
    block_add(&r->lap_blk, sample);
    if (r->wo && (!r->wo_done || r->seg_closing)) {
        block_add(&r->seg_blk, sample);
    }
    if (r->seg_closing) {
        block_close(r, &r->seg_blk, BLOCK_INTERVAL, r->seg_closing);
        r->seg_closing = 0;
    }
    int z = metrics_zone(sample->power_w, r->cfg.ftp_w);
    if (z >= 0) {
        r->zone_s[z]++;
    }
    r->n_samples++;
    metrics_add(&r->m, sample);
    r->p3[r->n_samples % 3] = sample->power_w;

    if (r->wo && !r->wo_done && *beeps == 0) {
        workout_pos_t pos;
        workout_at(r->wo, wo_time(r), &pos);
        if (pos.seg_left_s == 3 && pos.idx + 1 < r->wo->n) {
            *beeps = 1; // next segment in 3 s
        }
    }
    return true;
}

void ride_view(const ride_t *r, ride_view_t *v)
{
    memset(v, 0, sizeof(*v));
    control_cmd_t cmd;
    ride_command(r, &cmd);
    bool fresh = trainer_fresh(r);

    v->state = r->state;
    v->mode = cmd.kind == CMD_ERG ? MODE_ERG : MODE_FREE;
    v->timer_s = r->timer_ms / 1000;
    v->cadence_rpm10 = fresh ? r->trainer.cadence_rpm10 : -1;
    v->speed_kmh100 = fresh ? r->trainer.speed_kmh100 : -1;
    v->hr_bpm = current_hr(r);
    v->target_w = cmd.kind == CMD_ERG ? cmd.target_w : 0;
    v->grade = cmd.kind == CMD_SIM ? cmd.grade : 0;
    v->base_grade = r->base_grade;
    v->gear = r->gear + 1;
    v->gears = r->cfg.gears.n;
    v->intensity = r->intensity;
    v->lap = r->lap;
    v->auto_paused = r->state == RIDE_PAUSED && r->auto_paused;
    if (r->last_blk.kind != BLOCK_NONE && v->timer_s < r->last_blk.until_s) {
        v->last_blk = r->last_blk;
    }

    // 3 s power while riding, instant power otherwise
    v->power_w = fresh ? r->trainer.power_w : -1;
    if (r->state == RIDE_RUNNING && r->n_samples >= 3) {
        int sum = 0, n = 0;
        for (int i = 0; i < 3; i++) {
            if (r->p3[i] >= 0) {
                sum += r->p3[i];
                n++;
            }
        }
        v->power_w = n ? (int16_t)(sum / n) : -1;
    }
    v->zone = (int8_t)metrics_zone(v->power_w, r->cfg.ftp_w);
    v->zone_s = v->zone >= 0 ? r->zone_s[v->zone] : 0;

    if (r->wo) {
        workout_pos_t pos;
        workout_at(r->wo, wo_time(r), &pos);
        v->has_workout = true;
        v->seg_idx = pos.idx;
        v->seg_count = r->wo->n;
        v->seg_left_s = pos.seg_left_s;
        v->cadence_target = pos.done ? 0 : pos.cadence;
        v->wo_done = r->wo_done || pos.done;
        const goal_t *g = !v->wo_done && pos.kind == SEG_ERG ? workout_goal(r->wo, pos.idx) : NULL;
        if (g) {
            v->adaptive = true;
            v->hold_avg = g->hold_avg;
            v->hr_lo = g->hr_lo;
            v->hr_hi = g->hr_hi;
            v->cad_lo = g->cad_lo;
            v->cad_hi = g->cad_hi;
            v->hr_cue = r->adapt_seg == pos.idx ? r->hr_cue : 0;
            v->cad_cue = r->adapt_seg == pos.idx ? r->cad_cue : 0;
            v->avg_goal_w = g->hold_avg ? watts(r, pos.target_pm) : 0;
            v->seg_avg_w = r->adapt_seg == pos.idx && r->adapt_s ? (int16_t)(r->adapt_j / (int32_t)r->adapt_s) : -1;
        }
        v->ramp_test = r->wo->ramp_test;
        int ri = ramp_idx(r);
        v->ramp_step = (uint8_t)(ri < 0 ? 0 : ri - r->wo->ramp_first + 1);
        v->ramp_s = r->ramp_s;
    }
    metrics_summary(&r->m, r->cfg.ftp_w, &v->sum);
}
