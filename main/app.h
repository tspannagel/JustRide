// Control task: owns the ride, feeds the trainer, recorder and UI.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "jr_types.h"
#include "ride.h"
#include "settings.h"

typedef struct {
    bool trainer;
    bool hr;
    bool controller;
    bool clock_ok;
    bool sd_ok;
    bool saving;  // last ride is being written to the SD card
    bool saved;   // last ride was saved (false: shorter than 1 min or SD error)
    uint16_t ftp_suggest_w;  // after a ride: 95 % of its best 20 min if above the user's FTP, else 0
} app_status_t;

void app_start(void);

// Thread-safe snapshot for the UI
void app_get_view(ride_view_t *v, app_status_t *st);
void app_post_action(action_t a);

// Load a .zwo from the SD card and start it. Only while idle.
bool app_start_workout(const char *path);
// Turn Wi-Fi on, only while idle (checked under the same lock that starts a ride).
void app_web_start(void);

// Settings and user changes, only while idle. Saved to the SD card.
void app_get_settings(settings_t *s);
// Name and values of the active user; false (nothing changed) if any value is out of range.
bool app_edit_user(const user_t *u);
// Set the active user's FTP to status.ftp_suggest_w
void app_accept_ftp(void);
bool app_select_user(int idx);
bool app_add_user(const char *name);   // also selects it
bool app_delete_user(void);            // the active one; never the last
