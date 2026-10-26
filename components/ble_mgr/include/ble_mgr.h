// BLE central: scan, connect, GATT discovery, subscribe, reconnect.
// Drivers describe what they need; ble_mgr does the GATT work and calls back.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BLE_CHR_NOTIFY 0x01
#define BLE_CHR_INDICATE 0x02
#define BLE_CHR_REQUIRED 0x04
#define BLE_MAX_CHRS 4

typedef struct {
    uint16_t uuid16;
    uint8_t flags;  // BLE_CHR_*
} ble_chr_req_t;

typedef struct {
    const char *name;
    uint8_t role;               // source_t; one device per role
    uint16_t service16;         // matches(adv): advertised 16-bit service UUID
    ble_chr_req_t chrs[BLE_MAX_CHRS];
    uint8_t n_chrs;
    // Called from the NimBLE host task; keep them short.
    void (*on_ready)(uint16_t conn);
    void (*on_notify)(uint16_t conn, int chr_idx, const uint8_t *data, uint16_t len);
    void (*on_disconnect)(uint16_t conn);
} ble_drv_t;

void ble_mgr_start(const ble_drv_t *const *drivers, int n);

// Write with response to chrs[chr_idx] of the driver's connection.
int ble_mgr_write(uint16_t conn, const ble_drv_t *drv, int chr_idx, const void *data, uint16_t len);

bool ble_mgr_connected(uint8_t role);

// Forget remembered devices (NVS) and disconnect; the next match is paired.
void ble_mgr_forget(void);
