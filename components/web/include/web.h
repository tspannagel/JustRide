// Wi-Fi (off-bike only), HTTP server, mDNS (trainer.local), SNTP, OTA.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "settings.h"

void web_init(void);
// Join the configured network; falls back to an own access point after 15 s.
void web_start(const settings_t *s);
// Switches Wi-Fi off; cuts a running transfer at its next chunk.
void web_stop(void);
// A ride wants to start: new requests are refused (503), a running transfer may finish.
void web_closing(void);
// A file transfer (download, upload, OTA) is running: bytes so far and in all.
bool web_transfer(uint32_t *done, uint32_t *total);
bool web_running(void);
// Diagnostics: print nearby networks (name, signal, channel, security). Only while Wi-Fi is off.
void web_scan_print(void);
// Rider changes from the web app. The control task owns the settings, so main
// passes its functions in; each one refuses while a ride runs.
typedef struct {
    void (*get)(settings_t *s);
    bool (*select)(const char *id);
    bool (*add)(const char *name);  // also selects the new rider
    bool (*edit)(const user_t *u);  // by u->id; false if a value is out of range
    bool (*remove)(const char *id);
} web_users_t;
void web_set_users(const web_users_t *u);

// Diagnostics: time sync state and the current UTC time
void web_time_print(void);
// One-line status for the UI
void web_status(char *buf, size_t size);
// Link state for the UI's Wi-Fi icon. Joined: *bars = signal 1..3 (RSSI, refreshed every 5 s); else 0.
// Never blocks: while web_start/web_stop hold the lock it answers from the last value.
typedef enum { WEB_LINK_OFF, WEB_LINK_CONNECTING, WEB_LINK_AP, WEB_LINK_JOINED } web_link_t;
web_link_t web_link(int *bars);
// The firmware's web app onto the SD card, on the first start of a new firmware version
// (blocks ~2 s then; call after the SD card is mounted, before Wi-Fi and Bluetooth start)
void web_install_app(void);
// Looking for the home Wi-Fi, before its IP and before the own access point starts (<= 15 s)
bool web_joining(void);

// Firmware updates are refused unless someone at the device allowed them
// within the last 5 minutes (physical presence instead of a password).
void web_allow_ota(void);
bool web_ota_allowed(void);

// A firmware or web-app upload, for a panel on the device (the browser may be on the
// NAS or a phone: the rider sees here that something is happening). After it ends,
// the result stays for a few seconds; a firmware that was received restarts the device.
typedef enum { WEB_UPD_FIRMWARE, WEB_UPD_WEBAPP } web_upd_kind_t;
typedef enum { WEB_UPD_RUNNING, WEB_UPD_DONE, WEB_UPD_FAILED } web_upd_state_t;
typedef struct {
    web_upd_kind_t kind;
    web_upd_state_t state;
    uint32_t done, total;  // bytes of the current file
    uint16_t files;        // web app: files saved since the panel opened
    char err[48];          // WEB_UPD_FAILED: why
} web_update_t;
// False: nothing to show (no upload lately, or Wi-Fi off).
bool web_update(web_update_t *u);
