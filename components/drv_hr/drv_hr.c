#include "drv_hr.h"

#include "esp_timer.h"
#include "evbus.h"

static void on_ready(uint16_t conn) {}
static void on_disconnect(uint16_t conn) {}

static void on_notify(uint16_t conn, int idx, const uint8_t *d, uint16_t len)
{
    // Flags bit 0: HR is uint16 instead of uint8
    if (len < 2 || ((d[0] & 0x01) && len < 3)) {
        return;
    }
    int hr = d[0] & 0x01 ? d[1] | d[2] << 8 : d[1];
    evt_t e = {.type = EVT_METRICS};
    e.metrics.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    e.metrics.source = SRC_HR;
    e.metrics.power_w = e.metrics.cadence_rpm10 = -1;
    e.metrics.speed_kmh100 = -1;
    e.metrics.hr_bpm = (int16_t)(hr > 0 && hr < 250 ? hr : -1);
    evbus_post(&e);
}

static bool matches(const struct ble_hs_adv_fields *adv) { return ble_adv_has_uuid16(adv, 0x180D); }

const ble_drv_t drv_hr = {
    .name = "hr",
    .role = SRC_HR,
    .per_user = true,
    .matches = matches,
    .service = {.u16 = BLE_UUID16_INIT(0x180D)},
    .chrs = {{{.u16 = BLE_UUID16_INIT(0x2A37)}, BLE_CHR_NOTIFY | BLE_CHR_REQUIRED}},
    .n_chrs = 1,
    .on_ready = on_ready,
    .on_notify = on_notify,
    .on_disconnect = on_disconnect,
};
