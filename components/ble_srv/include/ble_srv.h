// BLE peripheral: JustRide re-broadcasts the trainer's power and cadence as a
// standard Cycling Power (0x1818) and Speed/Cadence (0x1816) sensor named
// "JustRide", so an Apple Watch (or a bike computer) records the ride too.
// Read-only: nothing can control the trainer through it.
#pragma once

#include <stdint.h>
#include "ble_mgr.h"

// Pass to ble_mgr_start()
extern const ble_server_t ble_srv;

// Latest values, about once per second from the control task. power_w < 0
// (unknown or stale) sends nothing, so receivers show a gap instead of a value.
void ble_srv_update(int16_t power_w, int16_t cadence_rpm10, uint32_t now_ms);
