#include "drv_ftms.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "evbus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ftms_ctl.h"
#include "proto_ftms.h"

static const char *TAG = "ftms";

#define CHR_BIKE_DATA 0
#define CHR_CONTROL 1
#define CHR_STATUS 2

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

typedef struct {
    uint8_t type;  // 0 = connected, 1 = lost, 2 = response, 3 = control permission lost
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
        if (d[0] == FTMS_STATUS_CONTROL_LOST) {
            ftms_msg_t m = {.type = 3, .conn = conn};
            xQueueSend(s_msgs, &m, 0);
        }
    }
}

void drv_ftms_set(const control_cmd_t *cmd) { xQueueOverwrite(s_desired, cmd); }

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void task(void *arg)
{
    uint16_t conn = 0xFFFF;
    control_cmd_t desired = {.kind = CMD_RESISTANCE, .level = 0};
    ftms_ctl_t ctl;
    ftms_ctl_init(&ctl);

    for (;;) {
        ftms_msg_t m;
        while (xQueueReceive(s_msgs, &m, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (m.type == 0) {
                conn = m.conn;
                ftms_ctl_on_connected(&ctl);
            } else if (m.type == 1) {
                conn = 0xFFFF;
                ftms_ctl_on_lost(&ctl);
            } else if (m.type == 3) {
                ESP_LOGW(TAG, "control lost, requesting it again");
                ftms_ctl_on_control_lost(&ctl, now_ms());
            } else {
                switch (ftms_ctl_on_response(&ctl, m.op, m.result, now_ms())) {
                case FTMS_CTL_RESP_CONTROL_LOST:
                    ESP_LOGW(TAG, "control lost, requesting it again");
                    break;
                case FTMS_CTL_RESP_REFUSED:
                    ESP_LOGW(TAG, "request control refused (%u)", m.result);
                    break;
                case FTMS_CTL_RESP_REJECTED:
                    if (m.op != FTMS_OP_START) {
                        ESP_LOGW(TAG, "op 0x%02x rejected (%u)", m.op, m.result);
                    }
                    break;
                default:
                    break;
                }
            }
        }
        xQueuePeek(s_desired, &desired, 0);

        uint8_t buf[FTMS_CP_MAX_LEN];
        size_t len = ftms_ctl_step(&ctl, &desired, now_ms(), buf);
        if (ctl.timed_out_op >= 0) {
            ESP_LOGW(TAG, "op 0x%02x timed out", ctl.timed_out_op);
        }
        if (len > 0 && ble_mgr_write(conn, &drv_ftms, CHR_CONTROL, buf, (uint16_t)len) != 0) {
            ftms_ctl_on_write_failed(&ctl);
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
