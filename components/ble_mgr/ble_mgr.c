#include "ble_mgr.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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
#define COOLDOWN_MS 60000  // a device that failed setup is skipped this long
#define MAX_COOL 4

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
    uint8_t props[BLE_MAX_CHRS];
    uint16_t all_def[MAX_ALL_CHRS];  // every characteristic declaration, to bound descriptor search
    int n_all;
    int sub_idx;      // characteristic being subscribed
    uint16_t cccd;
    uint32_t user_gen;  // s_user_gen when the connection was started
} link_t;

static const ble_drv_t *const *s_drv;
static int s_n_drv;
static const ble_server_t *s_server;
static link_t s_links[MAX_LINKS];
static uint8_t s_own_addr_type;
static bool s_connecting;
// Devices that just failed setup; skipped so they can't keep other roles from connecting
static struct {
    ble_addr_t addr;
    TickType_t until;
} s_cool[MAX_COOL];
static int s_cool_next;

static bool cooling(const ble_addr_t *a)
{
    for (int i = 0; i < MAX_COOL; i++) {
        if (s_cool[i].until && ble_addr_cmp(&s_cool[i].addr, a) == 0 &&
            (int32_t)(s_cool[i].until - xTaskGetTickCount()) > 0) {
            return true;
        }
    }
    return false;
}
static int s_fails;
static struct ble_npl_callout s_rescan;  // runs on the host task, like every other callback here
static struct ble_npl_event s_user_ev, s_forget_ev;
static bool s_started;
static char s_user[8];           // host task only (after start)
static char s_user_req[8];       // written by other tasks, guarded by s_mux
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_user_gen;      // bumped on every user change
static uint32_t s_connect_gen;   // s_user_gen of the connection being made

static int gap_event(struct ble_gap_event *ev, void *arg);
static void subscribe_next(link_t *l);

// ---- NVS: remembered device per role ------------------------------------------

// "role1" for shared devices, "u3.r2" for the active user's (NVS keys: max 15 chars)
static void role_key(const ble_drv_t *drv, char *key)
{
    if (drv->per_user) {
        snprintf(key, 16, "%.7s.r%u", s_user, drv->role);
    } else {
        snprintf(key, 16, "role%u", drv->role);
    }
}

static bool saved_addr(const ble_drv_t *drv, ble_addr_t *addr)
{
    nvs_handle_t h;
    char key[16];
    role_key(drv, key);
    size_t len = sizeof(*addr);
    if (nvs_open("ble", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_get_blob(h, key, addr, &len) == ESP_OK && len == sizeof(*addr);
    nvs_close(h);
    return ok;
}

static void save_addr(const ble_drv_t *drv, const ble_addr_t *addr)
{
    ble_addr_t old;
    if (saved_addr(drv, &old) && memcmp(&old, addr, sizeof(old)) == 0) {
        return;  // no flash write if unchanged
    }
    nvs_handle_t h;
    char key[16];
    role_key(drv, key);
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

static void schedule_rescan(void);

static void start_scan(void)
{
    if (s_connecting || all_roles_linked() || ble_gap_disc_active()) {
        return;
    }
    // 30 ms every 100 ms (units of 0.625 ms). The default (10 of 10 ms) keeps the shared
    // radio busy all the time and Wi-Fi clients lose the access point.
    struct ble_gap_disc_params p = {.itvl = 160, .window = 48, .filter_duplicates = 1};
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0) {
        // e.g. busy while a receiver connects: try again, or a missing sensor is never looked for
        ESP_LOGW(TAG, "scan start failed: %d, retrying", rc);
        schedule_rescan();
    }
}

static void rescan_cb(struct ble_npl_event *ev) { start_scan(); }

static void schedule_rescan(void)
{
    int delay_s = s_fails < 5 ? 1 << s_fails : BACKOFF_MAX_S;
    ble_npl_callout_reset(&s_rescan, ble_npl_time_ms_to_ticks32((uint32_t)delay_s * 1000));
}

bool ble_adv_has_uuid16(const struct ble_hs_adv_fields *f, uint16_t uuid)
{
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == uuid) {
            return true;
        }
    }
    return false;
}

bool ble_adv_has_company(const struct ble_hs_adv_fields *f, uint16_t company)
{
    return f->mfg_data && f->mfg_data_len >= 2 && (f->mfg_data[0] | f->mfg_data[1] << 8) == company;
}

static volatile bool s_dump;

void ble_mgr_dump(bool on)
{
    s_dump = on;
    if (on && ble_gap_disc_active()) {
        ble_gap_disc_cancel();  // restart so the duplicate filter reports every device again
        start_scan();
    }
}

static void on_disc(const struct ble_gap_disc_desc *d)
{
    if (s_dump) {
        // Raw advertising / scan-response bytes: ad structures are length, type, data
        char hex[2 * BLE_HS_ADV_MAX_SZ + 1];
        int n = d->length_data < BLE_HS_ADV_MAX_SZ ? d->length_data : BLE_HS_ADV_MAX_SZ;
        for (int i = 0; i < n; i++) {
            sprintf(&hex[2 * i], "%02x", d->data[i]);
        }
        hex[2 * n] = 0;
        const uint8_t *a = d->addr.val;
        ESP_LOGI(TAG, "adv %02x:%02x:%02x:%02x:%02x:%02x type %u rssi %d %s", a[5], a[4], a[3], a[2], a[1], a[0],
                 d->event_type, d->rssi, hex);
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return;
    }
    for (int i = 0; i < s_n_drv; i++) {
        const ble_drv_t *drv = s_drv[i];
        ble_addr_t want;
        if (link_by_role(drv->role) || !drv->matches(&f)) {
            continue;
        }
        if (saved_addr(drv, &want) && ble_addr_cmp(&want, &d->addr) != 0) {
            continue;  // a different device of this kind is remembered
        }
        if (cooling(&d->addr)) {
            continue;
        }
        ESP_LOGI(TAG, "connecting %s (rssi %d)", drv->name, d->rssi);
        ble_gap_disc_cancel();
        s_connecting = true;
        s_connect_gen = s_user_gen;
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
    ESP_LOGW(TAG, "%s: %s, disconnecting (skipped for %d s)", l->drv->name, why, COOLDOWN_MS / 1000);
    s_cool[s_cool_next].addr = l->addr;
    s_cool[s_cool_next].until = (xTaskGetTickCount() + pdMS_TO_TICKS(COOLDOWN_MS)) | 1;  // 0 = free slot
    s_cool_next = (s_cool_next + 1) % MAX_COOL;
    ble_gap_terminate(l->conn, BLE_ERR_REM_USER_CONN_TERM);
}

// The expected service is missing (e.g. after a firmware update): log what the device
// offers so the driver can be fixed from the log, then give up on it.
static int on_any_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    if (err->status == 0) {
        char u[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "%s offers service %s (handles %u-%u)", l->drv->name, ble_uuid_to_str(&svc->uuid.u, u),
                 svc->start_handle, svc->end_handle);
        return 0;
    }
    fail_link(l, "service not found");
    return 0;
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
    if (ble_gattc_write_flat(conn, l->cccd, v, sizeof(v), on_cccd_written, NULL) != 0) {
        fail_link(l, "subscribe");
    }
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
    if (drv->per_user && l->user_gen != s_user_gen) {
        fail_link(l, "user changed");  // never remember it for the wrong user
        return;
    }
    l->ready = true;
    s_fails = 0;
    ESP_LOGI(TAG, "%s ready", drv->name);
    save_addr(drv, &l->addr);
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
        char u[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "%s characteristic %s handle %u props 0x%02x", l->drv->name, ble_uuid_to_str(&chr->uuid.u, u),
                 chr->val_handle, chr->properties);
        if (l->n_all < MAX_ALL_CHRS) {
            l->all_def[l->n_all++] = chr->def_handle;
        }
        for (int i = 0; i < l->drv->n_chrs; i++) {
            if (ble_uuid_cmp(&chr->uuid.u, &l->drv->chrs[i].uuid.u) == 0) {
                l->val[i] = chr->val_handle;
                l->def[i] = chr->def_handle;
                l->props[i] = chr->properties;
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
    if (err->status == BLE_HS_EDONE && l->svc_start == 0) {
        if (ble_gattc_disc_all_svcs(conn, on_any_svc, NULL) != 0) {
            fail_link(l, "service not found");
        }
        return 0;
    }
    if (err->status != BLE_HS_EDONE) {
        fail_link(l, "service discovery failed");
        return 0;
    }
    if (ble_gattc_disc_all_chrs(conn, l->svc_start, l->svc_end, on_chr, NULL) != 0) {
        fail_link(l, "characteristic discovery");
    }
    return 0;
}

// Discovery starts only after the MTU exchange: run in parallel, it stalled until the
// 30 s GATT timeout on the Zwift Ride.
static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    link_t *l = link_by_conn(conn);
    if (!l) {
        return 0;
    }
    ESP_LOGI(TAG, "%s: MTU %u (status %d)", l->drv->name, mtu, err->status);
    // A procedure that fails to start never calls back: drop the link, or this role stays blocked
    if (ble_gattc_disc_svc_by_uuid(conn, &l->drv->service.u, on_svc, NULL) != 0) {
        fail_link(l, "service discovery");
    }
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
        l->user_gen = s_connect_gen;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(l->conn, &desc) == 0) {
            l->addr = desc.peer_id_addr;
        }
        // Ask for a bigger MTU (CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU); at the default 23 bytes
        // notifications carry only 20. on_mtu then starts service discovery.
        if (ble_gattc_exchange_mtu(l->conn, on_mtu, NULL) != 0) {
            fail_link(l, "MTU exchange");
        }
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
        uint8_t buf[256];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len) != 0) {
            ESP_LOGW(TAG, "%s: notification over %u bytes dropped", l->drv->name, (unsigned)sizeof(buf));
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
    if (!l || !l->ready || l->drv != drv || !l->val[chr_idx]) {
        return BLE_HS_ENOTCONN;
    }
    if (!(l->props[chr_idx] & BLE_GATT_CHR_PROP_WRITE)) {
        return ble_gattc_write_no_rsp_flat(conn, l->val[chr_idx], data, len);
    }
    return ble_gattc_write_flat(conn, l->val[chr_idx], data, len, NULL, NULL);
}

static void disconnect_all(bool per_user_only)
{
    for (int i = 0; i < MAX_LINKS; i++) {
        if (s_links[i].used && (!per_user_only || s_links[i].drv->per_user)) {
            ble_gap_terminate(s_links[i].conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

static void forget_cb(struct ble_npl_event *ev)
{
    nvs_handle_t h;
    if (nvs_open("ble", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < s_n_drv; i++) {
            char key[16];
            role_key(s_drv[i], key);
            nvs_erase_key(h, key);  // other users' devices stay remembered
        }
        nvs_commit(h);
        nvs_close(h);
    }
    disconnect_all(false);
}

static void user_cb(struct ble_npl_event *ev)
{
    portENTER_CRITICAL(&s_mux);
    bool changed = strcmp(s_user, s_user_req) != 0;
    memcpy(s_user, s_user_req, sizeof(s_user));
    portEXIT_CRITICAL(&s_mux);
    if (changed) {
        s_user_gen++;
        ESP_LOGI(TAG, "user %s", s_user);
        disconnect_all(true);
        start_scan();
    }
}

void ble_mgr_forget(void)
{
    if (s_started) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_forget_ev);
    }
}

void ble_mgr_set_user(const char *id)
{
    portENTER_CRITICAL(&s_mux);
    snprintf(s_user_req, sizeof(s_user_req), "%s", id);
    if (!s_started) {
        memcpy(s_user, s_user_req, sizeof(s_user));
    }
    portEXIT_CRITICAL(&s_mux);
    if (s_started) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_user_ev);
    }
}

// ---- Host ---------------------------------------------------------------------

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    start_scan();
    if (s_server) {
        s_server->sync(s_own_addr_type);
    }
}

static void on_reset(int reason) { ESP_LOGE(TAG, "host reset: %d", reason); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_mgr_start(const ble_drv_t *const *drivers, int n, const ble_server_t *server)
{
    s_drv = drivers;
    s_n_drv = n;
    s_server = server;
    ESP_ERROR_CHECK(nimble_port_init());
    if (server) {
        server->init();  // services must be registered before the host starts
    }
    ble_npl_callout_init(&s_rescan, nimble_port_get_dflt_eventq(), rescan_cb, NULL);
    ble_npl_event_init(&s_user_ev, user_cb, NULL);
    ble_npl_event_init(&s_forget_ev, forget_cb, NULL);
    s_started = true;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
