#include "ftms_ctl.h"

#include <string.h>
#include "proto_ftms.h"

void ftms_ctl_init(ftms_ctl_t *c)
{
    memset(c, 0, sizeof(*c));
    c->timed_out_op = -1;
}

void ftms_ctl_on_connected(ftms_ctl_t *c)
{
    c->stage = FTMS_CTL_NEED_CONTROL;
    c->in_flight = c->have_acked = false;
}

void ftms_ctl_on_lost(ftms_ctl_t *c)
{
    c->stage = FTMS_CTL_DOWN;
    c->in_flight = false;
}

void ftms_ctl_on_control_lost(ftms_ctl_t *c, uint32_t now_ms)
{
    // Another app (the KICKR allows 3 connections) took control: ask for it
    // again, then resend the current command. The pause avoids a tug of war
    // with an app that grabs control back right away.
    if (c->stage != FTMS_CTL_DOWN) {
        c->stage = FTMS_CTL_NEED_CONTROL;
    }
    c->have_acked = false;
    c->retry_ms = now_ms + FTMS_CTL_RETRY_MS;
}

ftms_ctl_resp_t ftms_ctl_on_response(ftms_ctl_t *c, uint8_t op, uint8_t result, uint32_t now_ms)
{
    if (!c->in_flight || op != c->in_flight_op) {
        return FTMS_CTL_RESP_IGNORED;
    }
    c->in_flight = false;
    if (op != FTMS_OP_REQUEST_CONTROL && result == FTMS_RESULT_CONTROL_NOT_PERMITTED) {
        ftms_ctl_on_control_lost(c, now_ms);
        return FTMS_CTL_RESP_CONTROL_LOST;
    }
    bool ok = result == FTMS_RESULT_SUCCESS;
    if (op == FTMS_OP_REQUEST_CONTROL) {
        if (ok) {
            c->stage = FTMS_CTL_NEED_START;
            return FTMS_CTL_RESP_OK;
        }
        c->retry_ms = now_ms + FTMS_CTL_RETRY_MS;
        return FTMS_CTL_RESP_REFUSED;
    }
    if (op == FTMS_OP_START) {
        c->stage = FTMS_CTL_RUNNING;  // some trainers refuse Start when already running
        return ok ? FTMS_CTL_RESP_OK : FTMS_CTL_RESP_REJECTED;
    }
    if (!ok && op == FTMS_OP_TARGET_RESISTANCE) {
        c->no_resistance = true;  // fall back to a flat SIM road
    }
    c->acked = c->sent;
    c->have_acked = ok || op != FTMS_OP_TARGET_RESISTANCE;
    return ok ? FTMS_CTL_RESP_OK : FTMS_CTL_RESP_REJECTED;
}

// Field-wise: memcmp would also compare padding bytes
static bool same_cmd(const control_cmd_t *a, const control_cmd_t *b)
{
    return a->kind == b->kind && a->target_w == b->target_w && a->grade == b->grade && a->crr == b->crr &&
           a->cw == b->cw && a->level == b->level;
}

static size_t encode(const control_cmd_t *c, uint8_t *buf)
{
    switch (c->kind) {
    case CMD_ERG:
        return ftms_cp_target_power(buf, c->target_w);
    case CMD_SIM:
        return ftms_cp_sim(buf, 0, c->grade, c->crr, c->cw);
    case CMD_RESISTANCE:
        return ftms_cp_target_resistance(buf, c->level);
    case CMD_START:
        return ftms_cp_start(buf);
    case CMD_PAUSE:
        return ftms_cp_stop(buf, true);
    case CMD_STOP:
        return ftms_cp_stop(buf, false);
    default:
        return 0;  // SPINDOWN deliberately not exposed
    }
}

size_t ftms_ctl_step(ftms_ctl_t *c, const control_cmd_t *desired, uint32_t now_ms, uint8_t *buf)
{
    c->timed_out_op = -1;
    if (c->in_flight && now_ms - c->sent_ms > FTMS_CTL_RESPONSE_TIMEOUT_MS) {
        c->timed_out_op = c->in_flight_op;
        c->in_flight = false;
    }
    if (c->stage == FTMS_CTL_DOWN || c->in_flight || now_ms - c->last_write_ms < FTMS_CTL_MIN_WRITE_GAP_MS ||
        (int32_t)(now_ms - c->retry_ms) < 0) {
        return 0;
    }

    control_cmd_t cmd = *desired;
    if (cmd.kind == CMD_RESISTANCE && c->no_resistance) {
        cmd = (control_cmd_t){.kind = CMD_SIM, .grade = 0, .crr = 40, .cw = 51};
    }
    size_t len = 0;
    if (c->stage == FTMS_CTL_NEED_CONTROL) {
        len = ftms_cp_request_control(buf);
    } else if (c->stage == FTMS_CTL_NEED_START) {
        len = ftms_cp_start(buf);
    } else if (!c->have_acked || !same_cmd(&cmd, &c->acked)) {
        if (cmd.kind == CMD_SIM && now_ms - c->last_sim_ms < FTMS_CTL_MIN_SIM_GAP_MS) {
            return 0;
        }
        len = encode(&cmd, buf);
        c->sent = cmd;
        if (cmd.kind == CMD_SIM) {
            c->last_sim_ms = now_ms;
        }
    }
    if (len == 0) {
        return 0;
    }
    c->in_flight = true;
    c->in_flight_op = buf[0];
    c->sent_ms = c->last_write_ms = now_ms;
    return len;
}

void ftms_ctl_on_write_failed(ftms_ctl_t *c) { c->in_flight = false; }
