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
} app_status_t;

void app_start(void);

// Thread-safe snapshot for the UI
void app_get_view(ride_view_t *v, app_status_t *st);
void app_post_action(action_t a);

// Load a .zwo from the SD card and start it. Only while idle.
bool app_start_workout(const char *path);
bool app_is_riding(void);

// Settings changes, only while idle. Saved to the SD card.
void app_get_settings(settings_t *s);
void app_set_ftp(uint16_t ftp_w);
void app_set_max_erg(uint16_t w);
