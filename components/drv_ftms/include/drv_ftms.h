// FTMS trainer driver (KICKR Core). Publishes metrics on evbus, applies the
// desired control command with one write in flight and rate limits.
#pragma once

#include "ble_mgr.h"
#include "jr_types.h"

extern const ble_drv_t drv_ftms;

void drv_ftms_init(void);

// Desired trainer state; latest wins. Sent when it changes and after reconnect.
void drv_ftms_set(const control_cmd_t *cmd);
