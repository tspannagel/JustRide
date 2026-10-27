// Ride recording on the SD card: per-second CSV (flushed every 10 s, doubles
// as the crash journal), FIT + summary + bests at the end.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "jr_types.h"
#include "metrics.h"

#define RIDES_DIR "/sdcard/rides"
#define RECORDER_MAX_S (8 * 3600)
#define RECORDER_MIN_S 60  // shorter rides are discarded

// Who rode: files go to RIDES_DIR/<user_id>/
typedef struct {
    char user_id[8];
    char workout[48];
    uint16_t ftp_w;
    uint8_t weight_kg;
    uint8_t max_hr;
} rec_info_t;

void recorder_init(void);
bool recorder_start(uint32_t start_utc, const rec_info_t *info);
void recorder_add(const ride_sample_t *s);
// Writes FIT, summary and bests. Returns false if nothing was saved.
// *ftp_estimate: 95 % of the ride's best 20 min, 0 if shorter.
bool recorder_finish(uint16_t *ftp_estimate);
// At boot: finish a ride that was interrupted by a crash or power loss.
void recorder_recover(void);
