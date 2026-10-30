// What JustRide re-broadcasts as a BLE sensor (so an Apple Watch or a bike
// computer can record power and cadence): Cycling Power (0x1818) and Cycling
// Speed and Cadence (0x1816) measurements. Pure C.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define CPS_MEAS_LEN 8
#define CSC_MEAS_LEN 5
#define CPS_FEATURE_CRANK 0x00000008u  // 0x2A65: crank revolution data supported
#define CSC_FEATURE_CRANK 0x0002u      // 0x2A5C: crank revolution data supported
#define CPS_LOCATION_REAR_HUB 13       // 0x2A5D

// The trainer only reports a cadence; receivers want crank revolutions and the
// time of the last one (1/1024 s). This turns one into the other.
typedef struct {
    uint32_t last_ms;    // time of the previous update, 0 = none yet
    uint32_t frac_mrev;  // revolution in progress, 1/1000
    uint16_t revs;       // cumulative, wraps
    uint16_t event_1024; // time of the last whole revolution, wraps
} crank_t;

// Advance to now_ms at the given cadence (0.1 rpm; <= 0: no pedalling).
// Gaps longer than 5 s (no data) don't add revolutions.
void crank_update(crank_t *c, uint32_t now_ms, int16_t cadence_rpm10);

// 0x2A63: flags, instantaneous power, crank revolutions, last crank event
size_t cps_encode_measurement(uint8_t *buf, int16_t power_w, const crank_t *c);
// 0x2A5B: flags, crank revolutions, last crank event
size_t csc_encode_measurement(uint8_t *buf, const crank_t *c);
