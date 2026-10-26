#include "drv_zap.h"

#include "esp_log.h"
#include "evbus.h"
#include "proto_zap.h"

static const char *TAG = "zap";

#define CHR_ASYNC 0
#define CHR_SYNC_RX 1
#define CHR_SYNC_TX 2
#define ZAP_UUID16 0xFC82  // Zwift 16-bit service UUID in some adverts (unverified)

// 0000000n-19CA-4651-86E5-FA29DCDD09D1, little-endian
#define ZAP_UUID(n) \
    {.u128 = BLE_UUID128_INIT(0xD1, 0x09, 0xDD, 0xDC, 0x29, 0xFA, 0xE5, 0x86, 0x51, 0x46, 0xCA, 0x19, n, 0x00, 0x00, 0x00)}

static bool s_dump;
static uint32_t s_pressed;

void drv_zap_dump(bool on) { s_dump = on; }

static bool matches(const struct ble_hs_adv_fields *adv)
{
    return ble_adv_has_company(adv, ZAP_COMPANY_ID) || ble_adv_has_uuid16(adv, ZAP_UUID16);
}

static void on_ready(uint16_t conn)
{
    s_pressed = 0;
    int rc = ble_mgr_write(conn, &drv_zap, CHR_SYNC_RX, ZAP_HANDSHAKE, ZAP_HANDSHAKE_LEN);
    ESP_LOGI(TAG, "handshake sent (rc %d)", rc);
}

static void on_disconnect(uint16_t conn)
{
    // No synthetic releases: a held button must never turn into a long press
    s_pressed = 0;
}

static void post(int id, uint8_t kind)
{
    evt_t e = {.type = EVT_INPUT, .input = {.id = (uint8_t)id, .kind = kind, .device = SRC_CONTROLLER}};
    evbus_post(&e);
}

static void on_notify(uint16_t conn, int idx, const uint8_t *d, uint16_t len)
{
    if (s_dump) {
        ESP_LOGI(TAG, "%s %u bytes:", idx == CHR_ASYNC ? "async" : "sync", len);
        ESP_LOG_BUFFER_HEX(TAG, d, len);
    }
    if (idx == CHR_SYNC_TX) {
        if (zap_is_handshake_reply(d, len)) {
            ESP_LOGI(TAG, "handshake ok");
        }
        return;
    }
    uint32_t map;
    if (idx != CHR_ASYNC || !zap_parse_ride_keypad(d, len, &map)) {
        return;
    }
    uint32_t now = zap_ride_pressed(map);
    uint32_t changed = now ^ s_pressed;
    s_pressed = now;
    for (uint32_t bit = 1; changed; bit <<= 1) {
        if (!(changed & bit)) {
            continue;
        }
        changed &= ~bit;
        int id = zap_ride_input(bit);
        if (id >= 0) {
            post(id, now & bit ? IN_PRESS : IN_RELEASE);
        }
    }
}

const ble_drv_t drv_zap = {
    .name = "zwift ride",
    .role = SRC_CONTROLLER,
    .matches = matches,
    .service = ZAP_UUID(0x01),
    .chrs = {
        {ZAP_UUID(0x02), BLE_CHR_NOTIFY | BLE_CHR_REQUIRED},    // Async
        {ZAP_UUID(0x03), BLE_CHR_REQUIRED},                     // SyncRX (write)
        {ZAP_UUID(0x04), BLE_CHR_INDICATE | BLE_CHR_REQUIRED},  // SyncTX
    },
    .n_chrs = 3,
    .on_ready = on_ready,
    .on_notify = on_notify,
    .on_disconnect = on_disconnect,
};
