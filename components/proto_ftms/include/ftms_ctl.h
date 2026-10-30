// FTMS control-point state machine. Pure C: takes connection events, control
// point responses, the desired command and time; returns the bytes to write.
// drv_ftms does the queues, BLE writes and logging.
//
// Rules: one write in flight (1 s response timeout), at most 4 writes/s, SIM at
// most 2/s. After connect: Request Control, Start, then the desired command.
// A rejected Target Resistance switches to a flat SIM road; other rejected
// commands are not retried until the desired command changes. When another app
// takes control, control is requested again after 2 s.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"

#define FTMS_CTL_RESPONSE_TIMEOUT_MS 1000
#define FTMS_CTL_MIN_WRITE_GAP_MS 250  // max 4 writes/s
#define FTMS_CTL_MIN_SIM_GAP_MS 500    // SIM grade max 2/s
#define FTMS_CTL_RETRY_MS 2000         // after control was lost or refused

typedef enum { FTMS_CTL_DOWN, FTMS_CTL_NEED_CONTROL, FTMS_CTL_NEED_START, FTMS_CTL_RUNNING } ftms_ctl_stage_t;

// What a control point response meant, for logging
typedef enum {
    FTMS_CTL_RESP_IGNORED,       // not for the write in flight
    FTMS_CTL_RESP_OK,
    FTMS_CTL_RESP_REFUSED,       // Request Control refused, retry after FTMS_CTL_RETRY_MS
    FTMS_CTL_RESP_REJECTED,      // command rejected
    FTMS_CTL_RESP_CONTROL_LOST,  // 0x05 control not permitted: re-request
} ftms_ctl_resp_t;

typedef struct {
    uint8_t stage;               // ftms_ctl_stage_t
    bool in_flight;
    uint8_t in_flight_op;
    int16_t timed_out_op;        // set by the last step: op that timed out, -1 none
    bool have_acked;
    bool no_resistance;          // trainer rejected Target Resistance
    control_cmd_t acked;         // last command the trainer accepted (or rejected for good)
    control_cmd_t sent;
    uint32_t sent_ms, last_write_ms, last_sim_ms, retry_ms;
    bool retry_pending;          // retry_ms is set (a time can't mean "none": ms wrap after 49.7 days)
} ftms_ctl_t;

void ftms_ctl_init(ftms_ctl_t *c);
void ftms_ctl_on_connected(ftms_ctl_t *c);
void ftms_ctl_on_lost(ftms_ctl_t *c);
ftms_ctl_resp_t ftms_ctl_on_response(ftms_ctl_t *c, uint8_t op, uint8_t result, uint32_t now_ms);
// Machine Status 0xFF: another client took control
void ftms_ctl_on_control_lost(ftms_ctl_t *c, uint32_t now_ms);

// Returns the number of bytes (≤ FTMS_CP_MAX_LEN) to write to the control
// point, or 0. A returned write counts as in flight.
size_t ftms_ctl_step(ftms_ctl_t *c, const control_cmd_t *desired, uint32_t now_ms, uint8_t *buf);
// The write returned by the last step could not be sent; the rate limit still applies.
void ftms_ctl_on_write_failed(ftms_ctl_t *c);
