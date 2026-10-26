// Zwift Ride controller (plain ZAP). Publishes button presses as input events.
#pragma once

#include <stdbool.h>
#include "ble_mgr.h"

extern const ble_drv_t drv_zap;

// Log every ZAP packet as hex (for captures / verifying button bits)
void drv_zap_dump(bool on);
