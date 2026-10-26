#include "drv_ftms.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "evbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "proto_ftms.h"

static const char *TAG = "ftms";

#define CHR_BIKE_DATA 0
#define CHR_CONTROL 1
#define CHR_STATUS 2

#define RESPONSE_TIMEOUT_MS 1000
#define MIN_WRITE_GAP_MS 250   // max 4 writes/s
#define MIN_SIM_GAP_MS 500     // SIM grade max 2/s
#define CONTROL_RETRY_MS 2000

static void on_ready(uint16_t conn);
static void on_notify(uint16_t conn, int idx, const uint8_t *d, uint16_t len);
static void on_disconnect(uint16_t conn);

static bool matches(const struct ble_hs_adv_fields *adv) { return ble_adv_has_uuid16(adv, 0x1826); }

const ble_drv_t drv_ftms = {
    .name = "trainer",
    .role = SRC_TRAINER,
    .matches = matches,
    .service = {.u16 = BLE_UUID16_INIT(0x1826)},
    .chrs = {
        {{.u16 = BLE_UUID16_INIT(0x2AD2)}, BLE_CHR_NOTIFY | BLE_CHR_REQUIRED},
        {{.u16 = BLE_UUID16_INIT(0x2AD9)}, BLE_CHR_INDICATE | BLE_CHR_REQUIRED},
        {{.u16 = BLE_UUID16_INIT(0x2ADA)}, BLE_CHR_NOTIFY},
    },
    .n_chrs = 3,
    .on_ready = on_ready,
    .on_notify = on_notify,
    .on_disconnect = on_disconnect,
};

typedef enum { ST_DOWN, ST_NEED_CONTROL, ST_NEED_START, ST_RUNNING } stage_t;

typedef struct {
    uint8_t type;  // 0 = connected, 1 = lost, 2 = response
    uint16_t conn;
    uint8_t op, result;
} ftms_msg_t;

static QueueHandle_t s_msgs;     // host task -> driver task
static QueueHandle_t s_desired;  // length-1 mailbox

static void on_ready(uint16_t conn)
{
    ftms_msg_t m = {.type = 0, .conn = conn};
    xQueueSend(s_msgs, &m, 0);
}

static void on_disconnect(uint16_t conn)
{
    ftms_msg_t m = {.type = 1, .conn = conn};
    xQueueSend(s_msgs, &m, 0);
}

static void on_notify(uint16_t conn, int idx, const uint8_t *d, uint16_t len)
{
    if (idx == CHR_BIKE_DATA) {
        ftms_bike_data_t b;
        if (!ftms_parse_bike_data(d, len, &b)) {
            return;
        }
        evt_t e = {.type = EVT_METRICS};
        e.metrics.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
        e.metrics.source = SRC_TRAINER;
        e.metrics.power_w = b.has & FTMS_HAS_POWER ? b.power_w : -1;
        e.metrics.cadence_rpm10 = b.has & FTMS_HAS_CADENCE ? (int16_t)b.cadence_rpm10 : -1;
        e.metrics.speed_kmh100 = b.has & FTMS_HAS_SPEED ? b.speed_kmh100 : -1;
        e.metrics.hr_bpm = b.has & FTMS_HAS_HR ? b.hr_bpm : -1;
        evbus_post(&e);
    } else if (idx == CHR_CONTROL) {
        ftms_msg_t m = {.type = 2, .conn = conn};
        if (ftms_parse_cp_response(d, len, &m.op, &m.result)) {
            xQueueSend(s_msgs, &m, 0);
        }
    } else if (idx == CHR_STATUS && len > 0) {
        ESP_LOGD(TAG, "machine status 0x%02x", d[0]);
    }
}

void drv_ftms_set(const control_cmd_t *cmd) { xQueueOverwrite(s_desired, cmd); }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

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

static void task(void *arg)
{
    uint16_t conn = 0xFFFF;
    stage_t stage = ST_DOWN;
    control_cmd_t desired = {.kind = CMD_RESISTANCE, .level = 0};
    control_cmd_t acked = {0};      // last command the trainer accepted (or rejected for good)
    control_cmd_t sent = {0};
    bool in_flight = false, have_acked = false, no_resistance = false;
    uint8_t in_flight_op = 0;
    uint32_t sent_ms = 0, last_write_ms = 0, last_sim_ms = 0, retry_ms = 0;

    for (;;) {
        ftms_msg_t m;
        while (xQueueReceive(s_msgs, &m, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (m.type == 0) {
                conn = m.conn;
                stage = ST_NEED_CONTROL;
                in_flight = have_acked = false;
            } else if (m.type == 1) {
                conn = 0xFFFF;
                stage = ST_DOWN;
                in_flight = false;
            } else if (in_flight && m.op == in_flight_op) {
                in_flight = false;
                bool ok = m.result == FTMS_RESULT_SUCCESS;
                if (m.op == FTMS_OP_REQUEST_CONTROL) {
                    if (ok) {
                        stage = ST_NEED_START;
                    } else {
                        ESP_LOGW(TAG, "request control refused (%u)", m.result);
                        retry_ms = now_ms() + CONTROL_RETRY_MS;
                    }
                } else if (m.op == FTMS_OP_START) {
                    stage = ST_RUNNING;  // some trainers refuse Start when already running
                } else {
                    if (!ok) {
                        ESP_LOGW(TAG, "op 0x%02x rejected (%u)", m.op, m.result);
                        if (m.op == FTMS_OP_TARGET_RESISTANCE) {
                            no_resistance = true;  // fall back to a flat SIM road
                        }
                    }
                    acked = sent;
                    have_acked = ok || m.op != FTMS_OP_TARGET_RESISTANCE;
                }
            }
        }
        xQueuePeek(s_desired, &desired, 0);

        uint32_t now = now_ms();
        if (in_flight && now - sent_ms > RESPONSE_TIMEOUT_MS) {
            ESP_LOGW(TAG, "op 0x%02x timed out", in_flight_op);
            in_flight = false;
        }
        if (stage == ST_DOWN || in_flight || now - last_write_ms < MIN_WRITE_GAP_MS || (int32_t)(now - retry_ms) < 0) {
            continue;
        }

        control_cmd_t c = desired;
        if (c.kind == CMD_RESISTANCE && no_resistance) {
            c = (control_cmd_t){.kind = CMD_SIM, .grade = 0, .crr = 40, .cw = 51};
        }
        uint8_t buf[FTMS_CP_MAX_LEN];
        size_t len = 0;
        if (stage == ST_NEED_CONTROL) {
            len = ftms_cp_request_control(buf);
        } else if (stage == ST_NEED_START) {
            len = ftms_cp_start(buf);
        } else if (!have_acked || !same_cmd(&c, &acked)) {
            if (c.kind == CMD_SIM && now - last_sim_ms < MIN_SIM_GAP_MS) {
                continue;
            }
            len = encode(&c, buf);
            sent = c;
            if (c.kind == CMD_SIM) {
                last_sim_ms = now;
            }
        }
        if (len == 0) {
            continue;
        }
        if (ble_mgr_write(conn, &drv_ftms, CHR_CONTROL, buf, (uint16_t)len) == 0) {
            in_flight = true;
            in_flight_op = buf[0];
            sent_ms = last_write_ms = now;
        } else {
            last_write_ms = now;
        }
    }
}

void drv_ftms_init(void)
{
    s_msgs = xQueueCreate(8, sizeof(ftms_msg_t));
    s_desired = xQueueCreate(1, sizeof(control_cmd_t));
    control_cmd_t safe = {.kind = CMD_RESISTANCE, .level = 0};
    xQueueOverwrite(s_desired, &safe);
    xTaskCreatePinnedToCore(task, "ftms", 4096, NULL, 6, NULL, 1);
}
