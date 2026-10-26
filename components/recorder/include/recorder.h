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

void recorder_init(void);
bool recorder_start(uint32_t start_utc, const char *workout, uint16_t ftp_w);
void recorder_add(const ride_sample_t *s);
// Writes FIT, summary and bests. Returns false if nothing was saved.
bool recorder_finish(void);
// At boot: finish a ride that was interrupted by a crash or power loss.
void recorder_recover(void);
