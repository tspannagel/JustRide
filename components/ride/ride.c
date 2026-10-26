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

static bool trainer_fresh(const ride_t *r)
{
    return r->trainer_ms != 0 && r->now_ms - r->trainer_ms <= RIDE_TRAINER_STALE_MS;
}

static int16_t current_hr(const ride_t *r)
{
    if (r->hr_ms != 0 && r->now_ms - r->hr_ms <= RIDE_HR_STALE_MS) {
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

static bool gear_mode(const ride_t *r)
{
    workout_pos_t pos;
    return !in_erg(r, &pos);
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
            r->lap++;
        }
        break;
    case ACT_PLUS:
    case ACT_MINUS: {
        int d = a == ACT_PLUS ? 1 : -1;
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
        if (r->wo && !r->wo_done && r->state == RIDE_RUNNING) {
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
        uint32_t w = (uint32_t)pos.target_pm * r->cfg.ftp_w / 1000 * r->intensity / 100;
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
        } else if (pos.idx != r->last_seg) {
            *beeps = r->last_seg < 0 ? 0 : 2;
            r->last_seg = pos.idx;
        }
    }

    // One sample per call; if a tick was late, later ticks catch up
    if (r->timer_ms / 1000 <= r->n_samples) {
        return false;
    }
    fill_sample(r, sample, utc);
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

    if (r->wo) {
        workout_pos_t pos;
        workout_at(r->wo, wo_time(r), &pos);
        v->has_workout = true;
        v->seg_idx = pos.idx;
        v->seg_count = r->wo->n;
        v->seg_left_s = pos.seg_left_s;
        v->cadence_target = pos.done ? 0 : pos.cadence;
        v->wo_done = r->wo_done || pos.done;
    }
    metrics_summary(&r->m, r->cfg.ftp_w, &v->sum);
}
