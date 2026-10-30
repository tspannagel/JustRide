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
    uint8_t receivers;  // watches / bike computers on the re-broadcast
    bool clock_ok;
    bool sd_ok;
    bool saving;  // last ride is being written to the SD card
    bool saved;   // last ride was saved (false: shorter than 1 min or SD error)
    // After a ride: 95 % of its best 20 min if above the user's FTP; after a ramp test:
    // 75 % of its best minute if different from the FTP. 0: nothing to offer.
    uint16_t ftp_suggest_w;
    bool ftp_test;           // the last ride was a ramp test
    int16_t ftp_test_60s_w;  // its best minute, -1 if there's no result (too short, not saved)
    uint16_t ftp_test_w;     // the FTP it gives (75 % of that minute), 0 if no result
} app_status_t;

void app_start(void);

// Thread-safe snapshot for the UI
void app_get_view(ride_view_t *v, app_status_t *st);
void app_post_action(action_t a);

// Parse a .zwo from the SD card (no side effects; for previews)
bool app_read_workout(const char *path, workout_t *w);
// Load a .zwo from the SD card and start it. Only while idle.
bool app_start_workout(const char *path);
// Start a workout built in memory (the adaptive quick start). Only while idle.
// A ramp test is rebuilt for the active rider first.
bool app_start_workout_mem(const workout_t *w);
// The FTP ramp test for the active rider (for the preview)
void app_ramp_test(workout_t *w);
// The workout of the current ride. Only changes while idle, so the UI may read
// it without a lock during a ride.
const workout_t *app_workout(void);
// Turn Wi-Fi on, only while idle (checked under the same lock that starts a ride).
void app_web_start(void);

// Settings and user changes, only while idle. Saved to the SD card.
void app_get_settings(settings_t *s);
// Users are addressed by id: the web app changes them too, so indexes can move.
// Name and values of user u->id; false (nothing changed) if any value is out of range.
bool app_edit_user(const user_t *u);
// Set the active user's FTP to status.ftp_suggest_w (and mark it known)
void app_accept_ftp(void);
bool app_select_user(const char *id);
bool app_add_user(const char *name);   // also selects it
bool app_delete_user(const char *id);  // never the last
// Time zone for ride names and FIT local time (a POSIX TZ rule, see tz.h)
bool app_set_tz(const char *posix);
