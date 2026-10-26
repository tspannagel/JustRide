// /config/settings.json and /config/gears.json on the SD card.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "vshift.h"

typedef struct {
    uint16_t ftp_w;
    uint16_t max_erg_w;
    uint8_t crr;          // 0.0001
    uint8_t cw;           // 0.01 kg/m
    char wifi_ssid[33];
    char wifi_pass[65];
    char ap_pass[65];     // generated on first boot
    gear_table_t gears;
} settings_t;

// Defaults for anything missing or invalid. Writes settings.json if it doesn't exist.
void settings_load(settings_t *s);
bool settings_save(const settings_t *s);
