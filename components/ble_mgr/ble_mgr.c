#include "ble_mgr.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "evbus.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

static const char *TAG = "ble";

#define MAX_LINKS 4
#define MAX_ALL_CHRS 16
#define CONNECT_TIMEOUT_MS 10000
#define BACKOFF_MAX_S 30
#define UUID_CCCD 0x2902
#define NO_CONN 0xFFFF

typedef struct {
    bool used;
    bool ready;
    uint16_t conn;
    const ble_drv_t *drv;
    ble_addr_t addr;
    uint16_t svc_start, svc_end;
    uint16_t val[BLE_MAX_CHRS];
    uint16_t def[BLE_MAX_CHRS];
    uint16_t end[BLE_MAX_CHRS];
    uint16_t all_def[MAX_ALL_CHRS];  // every characteristic declaration, to bound descriptor search
    int n_all;
    int sub_idx;      // characteristic being subscribed
    uint16_t cccd;
} link_t;

static const ble_drv_t *const *s_drv;
static int s_n_drv;
static link_t s_links[MAX_LINKS];
static uint8_t s_own_addr_type;
static bool s_connecting;
static int s_fails;
static esp_timer_handle_t s_rescan;

static int gap_event(struct ble_gap_event *ev, void *arg);
static void subscribe_next(link_t *l);

// ---- NVS: remembered device per role ------------------------------------------

static void role_key(uint8_t role, char *key) { snprintf(key, 8, "role%u", role); }

static bool saved_addr(uint8_t role, ble_addr_t *addr)
{
    nvs_handle_t h;
    char key[8];
    role_key(role, key);
    size_t len = sizeof(*addr);
    if (nvs_open("ble", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(h, key, addr, &len) == ESP_OK && len == sizeof(*addr);
    nvs_close(h);
    return ok;
}

static void save_addr(uint8_t role, const ble_addr_t *addr)
{
    ble_addr_t old;
    if (saved_addr(role, &old) && memcmp(&old, addr, sizeof(old)) == 0) {
        return;  // no flash write if unchanged
    }
    nvs_handle_t h;
    char key[8];
    role_key(role, key);
    if (nvs_open("ble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, addr, sizeof(*addr));
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---- Links --------------------------------------------------------------------

static link_t *link_by_conn(uint16_t conn)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].used && s_links[i].conn == conn) {
            return &s_links[i];
        }
    }
    return NULL;
}

static link_t *link_by_role(uint8_t role)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].used && s_links[i].drv->role == role) {
            return &s_links[i];
        }
    }
    return NULL;
}

bool ble_mgr_connected(uint8_t role)
{
    link_t *l = link_by_role(role);
    return l && l->ready;
}

static void post_device(uint8_t kind, uint8_t role)
{
    evt_t e = {.type = EVT_DEVICE, .device = {.kind = kind, .role = role}};
    evbus_post(&e);
}

// ---- Scanning -----------------------------------------------------------------

static bool all_roles_linked(void)
{
    for (int i = 0; i < s_n_drv; i++) {
        if (!link_by_role(s_drv[i]->role)) {
            return false;
        }
    }
    return true;
}

static void start_scan(void)
{
    if (s_connecting || all_roles_linked() || ble_gap_disc_active()) {
        return;
    }
    struct ble_gap_disc_params p = {.filter_duplicates = 1};
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
    }
}

static void rescan_cb(void *arg) { start_scan(); }

static void schedule_rescan(void)
{
    int delay_s = s_fails < 5 ? 1 << s_fails : BACKOFF_MAX_S;
    esp_timer_stop(s_rescan);
    esp_timer_start_once(s_rescan, (uint64_t)delay_s * 1000000);
}

static bool adv_has_service(const struct ble_hs_adv_fields *f, uint16_t uuid)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == uuid) {
            return true;
        }
    }
    return false;
}

static void on_disc(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return;
    }
    for (int i = 0; i < s_n_drv; i++) {
        const ble_drv_t *drv = s_drv[i];
        ble_addr_t want;
        if (link_by_role(drv->role) || !adv_has_service(&f, drv->service16)) {
            continue;
        }
        if (saved_addr(drv->role, &want) && ble_addr_cmp(&want, &d->addr) != 0) {
            continue;  // a different device of this kind is remembered
        }
        ESP_LOGI(TAG, "connecting %s (rssi %d)", drv->name, d->rssi);
        ble_gap_disc_cancel();
        s_connecting = true;
        if (ble_gap_connect(s_own_addr_type, &d->addr, CONNECT_TIMEOUT_MS, NULL, gap_event, (void *)drv) != 0) {
            s_connecting = false;
            schedule_rescan();
        }
        return;
    }
}

// ---- GATT discovery -----------------------------------------------------------

static void fail_link(link_t *l, const char *why)
{
    ESP_LOGW(TAG, "%s: %s, disconnecting", l->drv->name, why);
    ble_gap_terminate(l->conn, BLE_ERR_REM_USER_CONN_TERM);
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    if (err->status != 0) {
        fail_link(l, "subscribe failed");
        return 0;
    }
    l->sub_idx++;
    subscribe_next(l);
    return 0;
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    if (err->status == 0) {
        if (ble_uuid_u16(&dsc->uuid.u) == UUID_CCCD) {
            l->cccd = dsc->handle;
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE || l->cccd == 0) {
        fail_link(l, "no CCCD");
        return 0;
    }
    uint8_t flags = l->drv->chrs[l->sub_idx].flags;
    uint8_t v[2] = {flags & BLE_CHR_INDICATE ? 0x02 : 0x01, 0x00};
    ble_gattc_write_flat(conn, l->cccd, v, sizeof(v), on_cccd_written, NULL);
    return 0;
}

static void subscribe_next(link_t *l)
{
    const ble_drv_t *drv = l->drv;
    while (l->sub_idx < drv->n_chrs) {
        int i = l->sub_idx;
        if (l->val[i] && (drv->chrs[i].flags & (BLE_CHR_NOTIFY | BLE_CHR_INDICATE))) {
            l->cccd = 0;
            if (ble_gattc_disc_all_dscs(l->conn, l->val[i], l->end[i], on_dsc, NULL) != 0) {
                fail_link(l, "descriptor discovery");
            }
            return;
        }
        l->sub_idx++;
    }
    l->ready = true;
    s_fails = 0;
    ESP_LOGI(TAG, "%s ready", drv->name);
    save_addr(drv->role, &l->addr);
    drv->on_ready(l->conn);
    post_device(DEV_CONNECTED, drv->role);
    start_scan();  // look for the remaining roles
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    if (err->status == 0) {
        if (l->n_all < MAX_ALL_CHRS) {
            l->all_def[l->n_all++] = chr->def_handle;
        }
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            for (int i = 0; i < l->drv->n_chrs; i++) {
                if (l->drv->chrs[i].uuid16 == ble_uuid_u16(&chr->uuid.u)) {
                    l->val[i] = chr->val_handle;
                    l->def[i] = chr->def_handle;
                }
            }
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_link(l, "characteristic discovery");
        return 0;
    }
    for (int i = 0; i < l->drv->n_chrs; i++) {
        if (!l->val[i] && (l->drv->chrs[i].flags & BLE_CHR_REQUIRED)) {
            fail_link(l, "required characteristic missing");
            return 0;
        }
        // Descriptors end before the next characteristic declaration
        l->end[i] = l->svc_end;
        for (int k = 0; k < l->n_all; k++) {
            if (l->all_def[k] > l->def[i] && l->all_def[k] - 1 < l->end[i]) {
                l->end[i] = l->all_def[k] - 1;
            }
        }
    }
    l->sub_idx = 0;
    subscribe_next(l);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    if (err->status == 0) {
        l->svc_start = svc->start_handle;
        l->svc_end = svc->end_handle;
        return 0;
    }
    if (err->status != BLE_HS_EDONE || l->svc_start == 0) {
        fail_link(l, "service not found");
        return 0;
    }
    ble_gattc_disc_all_chrs(conn, l->svc_start, l->svc_end, on_chr, NULL);
    return 0;
}

// ---- GAP events ---------------------------------------------------------------

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        on_disc(&ev->disc);
        break;

    case BLE_GAP_EVENT_CONNECT: {
        s_connecting = false;
        const ble_drv_t *drv = arg;
        link_t *l = NULL;
        for (int i = 0; i < MAX_LINKS && !l; i++) {
            l = s_links[i].used ? NULL : &s_links[i];
        }
        if (ev->connect.status != 0 || !l) {
            ESP_LOGW(TAG, "connect %s failed: %d", drv->name, ev->connect.status);
            s_fails++;
            if (ev->connect.status == 0) {
                ble_gap_terminate(ev->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            schedule_rescan();
            break;
        }
        memset(l, 0, sizeof(*l));
        l->used = true;
        l->conn = ev->connect.conn_handle;
        l->drv = drv;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(l->conn, &desc) == 0) {
            l->addr = desc.peer_id_addr;
        }
        ble_gattc_disc_svc_by_uuid(l->conn, BLE_UUID16_DECLARE(drv->service16), on_svc, NULL);
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        link_t *l = link_by_conn(ev->disconnect.conn.conn_handle);
        if (l) {
            ESP_LOGW(TAG, "%s disconnected (reason 0x%x)", l->drv->name, ev->disconnect.reason);
            bool was_ready = l->ready;
            l->used = false;
            l->drv->on_disconnect(l->conn);
            if (was_ready) {
                post_device(DEV_LOST, l->drv->role);
            } else {
                s_fails++;
            }
        }
        schedule_rescan();
        break;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
        link_t *l = link_by_conn(ev->notify_rx.conn_handle);
        if (!l || !l->ready) {
            break;
        }
        uint8_t buf[64];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len) != 0) {
            break;
        }
        for (int i = 0; i < l->drv->n_chrs; i++) {
            if (l->val[i] == ev->notify_rx.attr_handle) {
                l->drv->on_notify(l->conn, i, buf, len);
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (!s_connecting) {
            schedule_rescan();
        }
        break;

    default:
        break;
    }
    return 0;
}

int ble_mgr_write(uint16_t conn, const ble_drv_t *drv, int chr_idx, const void *data, uint16_t len)
{
    link_t *l = link_by_conn(conn);
    if (!l || !l->ready || l->drv != drv) {
        return BLE_HS_ENOTCONN;
    }
    return ble_gattc_write_flat(conn, l->val[chr_idx], data, len, NULL, NULL);
}

void ble_mgr_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("ble", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].used) {
            ble_gap_terminate(s_links[i].conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

// ---- Host ---------------------------------------------------------------------

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset: %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_mgr_start(const ble_drv_t *const *drivers, int n)
{
    s_drv = drivers;
    s_n_drv = n;
    esp_timer_create_args_t t = {.callback = rescan_cb, .name = "ble_rescan"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_rescan));
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
