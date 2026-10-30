// Minimal FIT activity encoder (file_id, device_info, event, record, lap,
// session, activity). Pure C, writes through a callback.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"

// Returns false to abort.
typedef bool (*fit_write_fn)(void *ctx, const void *data, size_t len);

typedef struct {
    uint16_t ftp_w;
    uint16_t sw_version;  // x100
    int32_t utc_offset_s; // local time - UTC (activity local_timestamp)
} fit_meta_t;

// Writes a complete .FIT file for the samples. Returns false on write failure
// or if there are no samples.
bool fit_write_activity(fit_write_fn fn, void *ctx, const ride_sample_t *s, size_t n, const fit_meta_t *meta);

uint16_t fit_crc16(uint16_t crc, const void *data, size_t len);
