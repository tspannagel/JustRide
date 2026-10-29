// Wi-Fi (off-bike only), HTTP server, mDNS (trainer.local), SNTP, OTA.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "settings.h"

void web_init(void);
// Join the configured network; falls back to an own access point after 15 s.
void web_start(const settings_t *s);
void web_stop(void);
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

// Firmware updates are refused unless someone at the device allowed them
// within the last 5 minutes (physical presence instead of a password).
void web_allow_ota(void);
bool web_ota_allowed(void);
