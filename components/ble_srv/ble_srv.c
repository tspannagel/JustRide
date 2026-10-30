#include "ble_srv.h"

#include <string.h>
#include "esp_log.h"
#include "evbus.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "proto_cps.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_srv";

#define DEVICE_NAME "JustRide"
#define APPEARANCE_CYCLING_POWER 0x0484
#define MAX_PEERS 2  // e.g. a watch and a bike computer
#define NO_CONN 0xFFFF
// Radio time: values change once a second, so ask receivers for a slow link
// (90-120 ms; Apple's accessory rules: min >= 15 ms, max >= min + 15 ms,
// timeout 2-6 s) and advertise every 200-300 ms instead of NimBLE's 30-60 ms.
// Busy radio events delayed the display refresh (flickering lines).
#define CONN_ITVL_MIN 72   // 1.25 ms units: 90 ms
#define CONN_ITVL_MAX 96   // 120 ms
#define CONN_TIMEOUT 400   // 10 ms units: 4 s
#define ADV_ITVL_MIN 320   // 0.625 ms units: 200 ms
#define ADV_ITVL_MAX 480   // 300 ms

typedef struct {
    uint16_t conn;
    bool cps, csc;  // subscribed to the measurement
} peer_t;

// Host task only
static peer_t s_peers[MAX_PEERS] = {{.conn = NO_CONN}, {.conn = NO_CONN}};
static uint8_t s_own_addr_type;
static uint16_t s_cps_meas, s_csc_meas;  // value handles
static crank_t s_crank;

// Written by the control task, read on the host task
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    int16_t power_w, cadence_rpm10;
    uint32_t now_ms;
} s_latest;
static struct ble_npl_event s_update_ev;
static bool s_started;

static void advertise(void);

// ---- GATT ---------------------------------------------------------------------

static int append(struct ble_gatt_access_ctxt *ctxt, const void *d, size_t n)
{
    return os_mbuf_append(ctxt->om, d, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint16_t uuid = ble_uuid_u16(ctxt->chr->uuid);
    if (uuid == 0x2A65) {
        uint8_t f[4] = {(uint8_t)CPS_FEATURE_CRANK, 0, 0, 0};
        return append(ctxt, f, sizeof(f));
    }
    if (uuid == 0x2A5C) {
        uint8_t f[2] = {(uint8_t)CSC_FEATURE_CRANK, 0};
        return append(ctxt, f, sizeof(f));
    }
    if (uuid == 0x2A5D) {
        uint8_t loc = CPS_LOCATION_REAR_HUB;
        return append(ctxt, &loc, 1);
    }
    return BLE_ATT_ERR_UNLIKELY;  // measurements are notify-only
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1818),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2A63), .access_cb = access, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_cps_meas},
            {.uuid = BLE_UUID16_DECLARE(0x2A65), .access_cb = access, .flags = BLE_GATT_CHR_F_READ},
            {.uuid = BLE_UUID16_DECLARE(0x2A5D), .access_cb = access, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1816),
        .characteristics = (struct ble_gatt_chr_def[]){
            {.uuid = BLE_UUID16_DECLARE(0x2A5B), .access_cb = access, .flags = BLE_GATT_CHR_F_NOTIFY,
             .val_handle = &s_csc_meas},
            {.uuid = BLE_UUID16_DECLARE(0x2A5C), .access_cb = access, .flags = BLE_GATT_CHR_F_READ},
            {0},
        },
    },
    {0},
};

static void notify(uint16_t conn, uint16_t handle, const uint8_t *d, size_t n)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)n);
    if (om) {
        ble_gatts_notify_custom(conn, handle, om);  // takes the mbuf, also on error
    }
}

// Host task: advance the crank model and notify subscribers
static void update_cb(struct ble_npl_event *ev)
{
    portENTER_CRITICAL(&s_mux);
    int16_t power = s_latest.power_w, cad = s_latest.cadence_rpm10;
    uint32_t now = s_latest.now_ms;
    portEXIT_CRITICAL(&s_mux);

    crank_update(&s_crank, now, cad);
    if (power < 0) {
        return;
    }
    uint8_t cps[CPS_MEAS_LEN], csc[CSC_MEAS_LEN];
    cps_encode_measurement(cps, power, &s_crank);
    csc_encode_measurement(csc, &s_crank);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].conn != NO_CONN && s_peers[i].cps) {
            notify(s_peers[i].conn, s_cps_meas, cps, sizeof(cps));
        }
        if (s_peers[i].conn != NO_CONN && s_peers[i].csc) {
            notify(s_peers[i].conn, s_csc_meas, csc, sizeof(csc));
        }
    }
}

void ble_srv_update(int16_t power_w, int16_t cadence_rpm10, uint32_t now_ms)
{
    if (!s_started) {
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_latest.power_w = power_w;
    s_latest.cadence_rpm10 = cadence_rpm10;
    s_latest.now_ms = now_ms;
    portEXIT_CRITICAL(&s_mux);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_update_ev);
}

// ---- GAP (peripheral side; the central side is ble_mgr's) -----------------------

// Tell the control task how many receivers are connected (home screen indicator)
static void post_receivers(void)
{
    uint8_t n = 0;
    for (int i = 0; i < MAX_PEERS; i++) {
        n += s_peers[i].conn != NO_CONN;
    }
    evt_t e = {.type = EVT_DEVICE, .device = {.kind = n ? DEV_CONNECTED : DEV_LOST, .role = SRC_RECEIVER, .value = n}};
    evbus_post(&e);
}

static peer_t *peer(uint16_t conn)
{
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].conn == conn) {
            return &s_peers[i];
        }
    }
    return NULL;
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            peer_t *p = peer(NO_CONN);
            if (p) {
                *p = (peer_t){.conn = ev->connect.conn_handle};
                struct ble_gap_conn_desc d;
                if (ble_gap_conn_find(ev->connect.conn_handle, &d) == 0) {
                    ESP_LOGI(TAG, "receiver connected, interval %u.%02u ms", d.conn_itvl * 125 / 100, d.conn_itvl * 125 % 100);
                }
                post_receivers();
                struct ble_gap_upd_params up = {.itvl_min = CONN_ITVL_MIN, .itvl_max = CONN_ITVL_MAX,
                                                .latency = 0, .supervision_timeout = CONN_TIMEOUT};
                ble_gap_update_params(ev->connect.conn_handle, &up);  // the receiver may refuse
            } else {
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_CONN_LIMIT);
            }
        }
        advertise();  // for the next receiver, or again after a failed connection
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        peer_t *p = peer(ev->disconnect.conn.conn_handle);
        if (p) {
            *p = (peer_t){.conn = NO_CONN};
            ESP_LOGI(TAG, "receiver disconnected (reason 0x%x)", ev->disconnect.reason);
            post_receivers();
        }
        advertise();
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        peer_t *p = peer(ev->subscribe.conn_handle);
        if (p && ev->subscribe.attr_handle == s_cps_meas) {
            p->cps = ev->subscribe.cur_notify;
        } else if (p && ev->subscribe.attr_handle == s_csc_meas) {
            p->csc = ev->subscribe.cur_notify;
        }
        break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (peer(ev->conn_update.conn_handle) && ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0) {
            ESP_LOGI(TAG, "receiver link: interval %u.%02u ms, latency %u", d.conn_itvl * 125 / 100,
                     d.conn_itvl * 125 % 100, d.conn_latency);
        }
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (ble_gap_adv_active() || !peer(NO_CONN)) {
        return;  // already advertising, or no room for another receiver
    }
    static ble_uuid16_t uuids[] = {BLE_UUID16_INIT(0x1818), BLE_UUID16_INIT(0x1816)};
    struct ble_hs_adv_fields f = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids16 = uuids,
        .num_uuids16 = 2,
        .uuids16_is_complete = 1,
        .name = (uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
        .appearance = APPEARANCE_CYCLING_POWER,
        .appearance_is_present = 1,
    };
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_gap_adv_params p = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                   .itvl_min = ADV_ITVL_MIN, .itvl_max = ADV_ITVL_MAX};
    if (rc == 0) {
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising failed: %d", rc);
    }
}

static void srv_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ESP_ERROR_CHECK(ble_gatts_count_cfg(s_services));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(s_services));
    ble_npl_event_init(&s_update_ev, update_cb, NULL);
    s_started = true;
}

static void srv_sync(uint8_t own_addr_type)
{
    s_own_addr_type = own_addr_type;
    advertise();
}

const ble_server_t ble_srv = {.init = srv_init, .sync = srv_sync};
