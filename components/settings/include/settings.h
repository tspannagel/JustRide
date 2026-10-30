// /config/settings.json and /config/gears.json on the SD card.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "tz.h"
#include "vshift.h"

#define SETTINGS_MAX_USERS 8
#define SETTINGS_NAME_LEN 16  // bytes incl. NUL

typedef struct {
    char id[8];           // "u1", "u2", ...: folder under /rides and BLE key; never reused
    char name[SETTINGS_NAME_LEN];
    uint16_t ftp_w;
    uint16_t max_erg_w;
    uint8_t weight_kg;
    uint8_t max_hr;
} user_t;

typedef struct {
    user_t users[SETTINGS_MAX_USERS];
    uint8_t n_users;      // always >= 1
    uint8_t user;         // index of the active user
    uint16_t next_id;
    uint8_t crr;          // 0.0001
    uint8_t cw;           // 0.01 kg/m
    char wifi_ssid[33];   // up to 32 bytes
    char wifi_pass[64];   // WPA2 passphrase: up to 63 characters
    char ap_pass[64];     // generated on first boot
    char tz[TZ_MAX_LEN];  // POSIX TZ rule for local time (ride names), "UTC0" by default
    gear_table_t gears;
} settings_t;

// Defaults for anything missing or invalid. Writes settings.json if it doesn't
// exist (or lacks an AP password); a file that isn't valid JSON is left untouched.
void settings_load(settings_t *s);
bool settings_save(const settings_t *s);

// Only the riders from settings.json (for the web server; no defaults, never writes).
// s->n_users is 0 if the file is missing or invalid.
void settings_read_users(settings_t *s);

static inline user_t *settings_user(settings_t *s) { return &s->users[s->user]; }

// Adds a user with default values and makes it active. False if full or the name is invalid.
bool settings_add_user(settings_t *s, const char *name);
// Index of the user with this id, -1 if none
int settings_find_user(const settings_t *s, const char *id);
// Removes a user (never the last one); its rides stay on the SD card. The active
// user stays active; if it was removed, the first one becomes active.
bool settings_delete_user(settings_t *s, int idx);
// Printable ASCII, 1..15 characters, not only spaces.
bool settings_valid_name(const char *name);
