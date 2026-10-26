#include "evbus.h"

#include "freertos/queue.h"

#define EVBUS_LEN 32

static QueueHandle_t s_q;

void evbus_init(void) { s_q = xQueueCreate(EVBUS_LEN, sizeof(evt_t)); }

bool evbus_post(const evt_t *e) { return xQueueSend(s_q, e, 0) == pdTRUE; }

bool evbus_get(evt_t *e, TickType_t wait) { return xQueueReceive(s_q, e, wait) == pdTRUE; }
