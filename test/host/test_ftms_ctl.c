// FTMS control-point state machine: sequencing, rate limits, rejections.
#include "ftms_ctl.h"
#include "proto_ftms.h"
#include "test.h"

static const control_cmd_t RES0 = {.kind = CMD_RESISTANCE, .level = 0};

static control_cmd_t erg(int16_t w) { return (control_cmd_t){.kind = CMD_ERG, .target_w = w}; }
static control_cmd_t sim(int16_t grade) { return (control_cmd_t){.kind = CMD_SIM, .grade = grade, .crr = 40, .cw = 51}; }

// Step once; returns the op written or -1
static int step(ftms_ctl_t *c, const control_cmd_t *d, uint32_t now, uint8_t *buf)
{
    size_t len = ftms_ctl_step(c, d, now, buf);
    CHECK(len <= FTMS_CP_MAX_LEN);
    return len ? buf[0] : -1;
}

// Connect, Request Control, Start, both acked; returns the time after that.
static uint32_t bring_up(ftms_ctl_t *c, uint32_t t)
{
    uint8_t buf[FTMS_CP_MAX_LEN];
    ftms_ctl_init(c);
    ftms_ctl_on_connected(c);
    CHECK_EQ(step(c, &RES0, t, buf), FTMS_OP_REQUEST_CONTROL);
    CHECK_EQ(ftms_ctl_on_response(c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_SUCCESS, t + 20), FTMS_CTL_RESP_OK);
    t += 250;
    CHECK_EQ(step(c, &RES0, t, buf), FTMS_OP_START);
    CHECK_EQ(ftms_ctl_on_response(c, FTMS_OP_START, FTMS_RESULT_SUCCESS, t + 20), FTMS_CTL_RESP_OK);
    CHECK_EQ(c->stage, FTMS_CTL_RUNNING);
    return t + 250;
}

static void test_nothing_while_down(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    ftms_ctl_init(&c);
    for (uint32_t t = 1000; t < 5000; t += 50) {
        CHECK_EQ(step(&c, &RES0, t, buf), -1);
    }
}

static void test_connect_sequence(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    // then the desired command: lowest resistance
    CHECK_EQ(ftms_ctl_step(&c, &RES0, t, buf), 2);
    const uint8_t want[] = {FTMS_OP_TARGET_RESISTANCE, 0};
    CHECK_BYTES(buf, want, 2);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_RESISTANCE, FTMS_RESULT_SUCCESS, t + 20), FTMS_CTL_RESP_OK);
    // acked: nothing more until it changes
    for (t += 250; t < 10000; t += 50) {
        CHECK_EQ(step(&c, &RES0, t, buf), -1);
    }
}

static void test_start_refused_still_runs(void)
{
    // Some trainers refuse Start when already running
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    ftms_ctl_init(&c);
    ftms_ctl_on_connected(&c);
    CHECK_EQ(step(&c, &RES0, 1000, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_response(&c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_SUCCESS, 1010);
    CHECK_EQ(step(&c, &RES0, 1250, buf), FTMS_OP_START);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_START, 0x04, 1260), FTMS_CTL_RESP_REJECTED);
    CHECK_EQ(c.stage, FTMS_CTL_RUNNING);
    CHECK_EQ(step(&c, &RES0, 1500, buf), FTMS_OP_TARGET_RESISTANCE);
}

static void test_request_control_refused(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    ftms_ctl_init(&c);
    ftms_ctl_on_connected(&c);
    CHECK_EQ(step(&c, &RES0, 1000, buf), FTMS_OP_REQUEST_CONTROL);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_CONTROL_NOT_PERMITTED, 1100),
             FTMS_CTL_RESP_REFUSED);
    for (uint32_t t = 1150; t < 3100; t += 50) {
        CHECK_EQ(step(&c, &RES0, t, buf), -1);
    }
    CHECK_EQ(step(&c, &RES0, 3100, buf), FTMS_OP_REQUEST_CONTROL);
}

static void test_write_rate_limit(void)
{
    // ERG target changes every 10 ms: at most one write per 250 ms (4/s)
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t0 = bring_up(&c, 1000), last = 0;
    int writes = 0;
    for (uint32_t t = t0; t < t0 + 2000; t += 10) {
        control_cmd_t d = erg((int16_t)(100 + t % 200));
        if (step(&c, &d, t, buf) >= 0) {
            if (writes) {
                CHECK(t - last >= FTMS_CTL_MIN_WRITE_GAP_MS);
            }
            last = t;
            writes++;
            ftms_ctl_on_response(&c, buf[0], FTMS_RESULT_SUCCESS, t + 5);
        }
    }
    CHECK_EQ(writes, 8);
}

static void test_sim_rate_limit(void)
{
    // SIM grade changes every 10 ms: at most one write per 500 ms (2/s)
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t0 = bring_up(&c, 1000), last = 0;
    int writes = 0;
    for (uint32_t t = t0; t < t0 + 2000; t += 10) {
        control_cmd_t d = sim((int16_t)(t / 10 % 500));
        if (step(&c, &d, t, buf) >= 0) {
            CHECK_EQ(buf[0], FTMS_OP_SIM);
            if (writes) {
                CHECK(t - last >= FTMS_CTL_MIN_SIM_GAP_MS);
            }
            last = t;
            writes++;
            ftms_ctl_on_response(&c, buf[0], FTMS_RESULT_SUCCESS, t + 5);
        }
    }
    CHECK_EQ(writes, 4);
}

static void test_one_in_flight_and_timeout(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(150), b = erg(200);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    // no response: nothing else goes out for 1 s, even with a new target
    for (uint32_t u = t + 50; u <= t + FTMS_CTL_RESPONSE_TIMEOUT_MS; u += 50) {
        CHECK_EQ(step(&c, &b, u, buf), -1);
        CHECK_EQ(c.timed_out_op, -1);
    }
    // timeout: reported and the current target goes out in the same step
    CHECK_EQ(step(&c, &b, t + 1050, buf), FTMS_OP_TARGET_POWER);
    CHECK_EQ(c.timed_out_op, FTMS_OP_TARGET_POWER);
    const uint8_t want[] = {FTMS_OP_TARGET_POWER, 200, 0};
    CHECK_BYTES(buf, want, 3);
    // a late response to the timed-out write is matched by op only (like the driver)
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 1100), FTMS_CTL_RESP_OK);
    CHECK_EQ(step(&c, &b, t + 1400, buf), -1);
}

static void test_unrelated_response_ignored(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(150);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_SIM, FTMS_RESULT_SUCCESS, t + 10), FTMS_CTL_RESP_IGNORED);
    CHECK(c.in_flight);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 20);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 30), FTMS_CTL_RESP_IGNORED);
}

static void test_write_failed(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(150);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    ftms_ctl_on_write_failed(&c);
    CHECK(!c.in_flight);
    CHECK_EQ(step(&c, &a, t + 200, buf), -1);  // rate limit still applies
    CHECK_EQ(step(&c, &a, t + 250, buf), FTMS_OP_TARGET_POWER);
}

static void test_reconnect_resends_target(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(220);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 20);

    // link drops with nothing in flight, and again with a write in flight
    ftms_ctl_on_lost(&c);
    CHECK_EQ(step(&c, &a, t + 5000, buf), -1);
    ftms_ctl_on_connected(&c);
    t += 6000;
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_lost(&c);
    CHECK(!c.in_flight);
    ftms_ctl_on_connected(&c);

    CHECK_EQ(step(&c, &a, t + 250, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_response(&c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_SUCCESS, t + 260);
    CHECK_EQ(step(&c, &a, t + 500, buf), FTMS_OP_START);
    ftms_ctl_on_response(&c, FTMS_OP_START, FTMS_RESULT_SUCCESS, t + 510);
    // same target as before the drop is sent again
    CHECK_EQ(ftms_ctl_step(&c, &a, t + 750, buf), 3);
    const uint8_t want[] = {FTMS_OP_TARGET_POWER, 220, 0};
    CHECK_BYTES(buf, want, 3);
}

static void test_resistance_rejected_flat_sim(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    CHECK_EQ(step(&c, &RES0, t, buf), FTMS_OP_TARGET_RESISTANCE);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_RESISTANCE, 0x02, t + 20), FTMS_CTL_RESP_REJECTED);
    // flat SIM road instead: grade 0, Crr 0.0040, Cw 0.51
    CHECK_EQ(ftms_ctl_step(&c, &RES0, t + 250, buf), 7);
    const uint8_t want[] = {FTMS_OP_SIM, 0, 0, 0, 0, 40, 51};
    CHECK_BYTES(buf, want, 7);
    ftms_ctl_on_response(&c, FTMS_OP_SIM, FTMS_RESULT_SUCCESS, t + 260);
    CHECK_EQ(step(&c, &RES0, t + 1000, buf), -1);
    // remembered: after ERG, back to resistance goes straight to the flat road
    control_cmd_t a = erg(150);
    CHECK_EQ(step(&c, &a, t + 1250, buf), FTMS_OP_TARGET_POWER);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 1260);
    CHECK_EQ(step(&c, &RES0, t + 1500, buf), FTMS_OP_SIM);
}

static void test_other_rejection_not_retried(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(300), b = erg(310);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, 0x03, t + 20), FTMS_CTL_RESP_REJECTED);
    for (uint32_t u = t + 250; u < t + 5000; u += 50) {
        CHECK_EQ(step(&c, &a, u, buf), -1);
    }
    // a new target is sent
    CHECK_EQ(step(&c, &b, t + 5000, buf), FTMS_OP_TARGET_POWER);
}

static void check_control_retaken(ftms_ctl_t *c, uint32_t lost_at, const control_cmd_t *d, uint8_t want_op)
{
    uint8_t buf[FTMS_CP_MAX_LEN];
    CHECK_EQ(c->stage, FTMS_CTL_NEED_CONTROL);
    for (uint32_t u = lost_at + 50; u < lost_at + FTMS_CTL_RETRY_MS; u += 50) {
        CHECK_EQ(step(c, d, u, buf), -1);
    }
    uint32_t t = lost_at + FTMS_CTL_RETRY_MS;
    CHECK_EQ(step(c, d, t, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_response(c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_SUCCESS, t + 10);
    CHECK_EQ(step(c, d, t + 250, buf), FTMS_OP_START);
    ftms_ctl_on_response(c, FTMS_OP_START, FTMS_RESULT_SUCCESS, t + 260);
    CHECK_EQ(step(c, d, t + 500, buf), want_op);  // current command again
}

static void test_control_lost_result(void)
{
    // Result 0x05 on a command: another app took control
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(180);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 10);
    a = erg(190);
    CHECK_EQ(step(&c, &a, t + 250, buf), FTMS_OP_TARGET_POWER);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_CONTROL_NOT_PERMITTED, t + 300),
             FTMS_CTL_RESP_CONTROL_LOST);
    CHECK(!c.in_flight);
    check_control_retaken(&c, t + 300, &a, FTMS_OP_TARGET_POWER);
}

static void test_control_lost_status(void)
{
    // Machine Status 0xFF while the last command was acked
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t s = sim(300);
    CHECK_EQ(step(&c, &s, t, buf), FTMS_OP_SIM);
    ftms_ctl_on_response(&c, FTMS_OP_SIM, FTMS_RESULT_SUCCESS, t + 10);
    ftms_ctl_on_control_lost(&c, t + 1000);
    check_control_retaken(&c, t + 1000, &s, FTMS_OP_SIM);
}

static void test_control_lost_while_down(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    ftms_ctl_init(&c);
    ftms_ctl_on_control_lost(&c, 1000);
    CHECK_EQ(c.stage, FTMS_CTL_DOWN);
    CHECK_EQ(step(&c, &RES0, 5000, buf), -1);
}

static void test_idle_and_pause_reach_trainer(void)
{
    // Ride idle/pause/stop must still be applied after ERG (safety: lowest load)
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t a = erg(400);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 10);

    control_cmd_t pause = {.kind = CMD_PAUSE};
    CHECK_EQ(ftms_ctl_step(&c, &pause, t + 250, buf), 2);
    const uint8_t want_pause[] = {FTMS_OP_STOP, 2};
    CHECK_BYTES(buf, want_pause, 2);
    ftms_ctl_on_response(&c, FTMS_OP_STOP, FTMS_RESULT_SUCCESS, t + 260);

    CHECK_EQ(ftms_ctl_step(&c, &RES0, t + 500, buf), 2);
    const uint8_t want_res[] = {FTMS_OP_TARGET_RESISTANCE, 0};
    CHECK_BYTES(buf, want_res, 2);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_RESISTANCE, FTMS_RESULT_SUCCESS, t + 510);

    control_cmd_t stop = {.kind = CMD_STOP};
    CHECK_EQ(ftms_ctl_step(&c, &stop, t + 750, buf), 2);
    const uint8_t want_stop[] = {FTMS_OP_STOP, 1};
    CHECK_BYTES(buf, want_stop, 2);
}

static void test_idle_after_rejected_pause(void)
{
    // A rejected Stop/Pause must not block the following idle command
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t pause = {.kind = CMD_PAUSE};
    CHECK_EQ(step(&c, &pause, t, buf), FTMS_OP_STOP);
    CHECK_EQ(ftms_ctl_on_response(&c, FTMS_OP_STOP, 0x02, t + 10), FTMS_CTL_RESP_REJECTED);
    CHECK_EQ(step(&c, &RES0, t + 250, buf), FTMS_OP_TARGET_RESISTANCE);
}

static void test_spindown_not_sent(void)
{
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    uint32_t t = bring_up(&c, 1000);
    control_cmd_t sd = {.kind = CMD_SPINDOWN};
    for (uint32_t u = t; u < t + 3000; u += 50) {
        CHECK_EQ(step(&c, &sd, u, buf), -1);
    }
}

static void test_clock_wrap(void)
{
    // esp_timer ms in uint32_t wraps after ~49.7 days. A trainer that connects
    // after ~24.8 days of uptime must still get Request Control right away
    // (an unset retry time used to read as "in the future" until the wrap).
    ftms_ctl_t c;
    uint8_t buf[FTMS_CP_MAX_LEN];
    for (uint32_t t0 = 0x80000000u; t0 >= 0x80000000u; t0 += 0x10000000u) {
        ftms_ctl_init(&c);
        ftms_ctl_on_connected(&c);
        CHECK_EQ(step(&c, &RES0, t0, buf), FTMS_OP_REQUEST_CONTROL);
    }
    ftms_ctl_init(&c);
    ftms_ctl_on_connected(&c);
    uint32_t t = UINT32_MAX - 3000;
    CHECK_EQ(step(&c, &RES0, t, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_response(&c, FTMS_OP_REQUEST_CONTROL, 0x05, t + 10);  // refused: retry in 2 s
    CHECK_EQ(step(&c, &RES0, t + 1000, buf), -1);
    ftms_ctl_on_control_lost(&c, t);
    t += FTMS_CTL_RETRY_MS;
    CHECK_EQ(step(&c, &RES0, t, buf), FTMS_OP_REQUEST_CONTROL);
    ftms_ctl_on_response(&c, FTMS_OP_REQUEST_CONTROL, FTMS_RESULT_SUCCESS, t + 10);
    CHECK_EQ(step(&c, &RES0, t + 250, buf), FTMS_OP_START);
    ftms_ctl_on_response(&c, FTMS_OP_START, FTMS_RESULT_SUCCESS, t + 260);
    t += 500;  // UINT32_MAX - 500
    control_cmd_t a = erg(150);
    CHECK_EQ(step(&c, &a, t, buf), FTMS_OP_TARGET_POWER);
    CHECK_EQ(step(&c, &a, t + 1000, buf), -1);                     // across the wrap, still in flight
    CHECK_EQ(step(&c, &a, t + 1050, buf), FTMS_OP_TARGET_POWER);  // timed out
    CHECK_EQ(c.timed_out_op, FTMS_OP_TARGET_POWER);
    ftms_ctl_on_response(&c, FTMS_OP_TARGET_POWER, FTMS_RESULT_SUCCESS, t + 1060);
    ftms_ctl_on_control_lost(&c, t + 1100);
    CHECK_EQ(step(&c, &a, t + 1100 + FTMS_CTL_RETRY_MS - 50, buf), -1);
    CHECK_EQ(step(&c, &a, t + 1100 + FTMS_CTL_RETRY_MS, buf), FTMS_OP_REQUEST_CONTROL);
}

int main(void)
{
    test_nothing_while_down();
    test_connect_sequence();
    test_start_refused_still_runs();
    test_request_control_refused();
    test_write_rate_limit();
    test_sim_rate_limit();
    test_one_in_flight_and_timeout();
    test_unrelated_response_ignored();
    test_write_failed();
    test_reconnect_resends_target();
    test_resistance_rejected_flat_sim();
    test_other_rejection_not_retried();
    test_control_lost_result();
    test_control_lost_status();
    test_control_lost_while_down();
    test_idle_and_pause_reach_trainer();
    test_idle_after_rejected_pause();
    test_spindown_not_sent();
    test_clock_wrap();
    return test_summary("ftms_ctl");
}
