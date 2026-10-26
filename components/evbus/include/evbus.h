// One event queue, consumed by the control task. Drivers and the UI post here.
// Nothing above evbus touches a BLE characteristic.
#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "jr_types.h"

typedef enum { EVT_METRICS, EVT_INPUT, EVT_DEVICE, EVT_ACTION } evt_type_t;

typedef struct {
    uint8_t type;  // evt_type_t
    union {
        metrics_evt_t metrics;
        input_evt_t input;
        device_evt_t device;
        uint8_t action;  // action_t
    };
} evt_t;

void evbus_init(void);
// Never blocks; returns false (and the event is dropped) if the queue is full.
bool evbus_post(const evt_t *e);
bool evbus_get(evt_t *e, TickType_t wait);
