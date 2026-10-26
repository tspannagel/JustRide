// CSV formats for ride files. Pure C. The per-second CSV doubles as the crash
// journal, so it can also be parsed back.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "jr_types.h"
#include "metrics.h"

#define CSV_RIDE_HEADER "t_s,ts_utc,power_w,cadence_rpm,speed_kmh,hr_bpm,target_w,grade_pct,gear,interval_idx,mode,lap\n"
#define CSV_SUMMARY_HEADER                                                                                     \
    "ride_id,start_utc,duration_s,moving_s,workout,ftp_w,avg_w,np_w,if,tss,kj,avg_hr,max_hr,avg_cad,distance_km," \
    "fw\n"
#define CSV_BESTS_HEADER "ride_id,duration_s,watts\n"
#define CSV_ROW_MAX 128

// Returns the length written (always NUL-terminated), 0 if buf is too small.
size_t csv_format_row(char *buf, size_t size, const ride_sample_t *s);
bool csv_parse_row(const char *line, ride_sample_t *s);

size_t csv_format_summary(char *buf, size_t size, const char *ride_id, uint32_t start_utc, const char *workout,
                          uint16_t ftp_w, const metrics_summary_t *m, const char *fw);
// All best-effort rows for one ride (skips durations longer than the ride).
size_t csv_format_bests(char *buf, size_t size, const char *ride_id, const int16_t bests[METRICS_N_BESTS]);

// "2026-10-26T12:00:00Z"; buf >= 21 bytes
void csv_iso_utc(char *buf, uint32_t t);
// "2026-10-26_1200" (ride id / file stem); buf >= 16 bytes
void csv_ride_id(char *buf, uint32_t t);
