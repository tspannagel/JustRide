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
    "fw,weight_kg,user_max_hr\n"
#define CSV_BESTS_HEADER "ride_id,duration_s,watts\n"
#define CSV_ROW_MAX 128

// Returns the length written (always NUL-terminated), 0 if buf is too small.
size_t csv_format_row(char *buf, size_t size, const ride_sample_t *s);
bool csv_parse_row(const char *line, ride_sample_t *s);

size_t csv_format_summary(char *buf, size_t size, const char *ride_id, uint32_t start_utc, const char *workout,
                          uint16_t ftp_w, const metrics_summary_t *m, const char *fw, uint8_t weight_kg,
                          uint8_t max_hr);
// Start time and TSS (0.1) of one summary.csv row (the home screen's week). The workout
// name is quoted and may hold commas. False for the header or a malformed row.
bool csv_parse_summary(const char *line, uint32_t *start_utc, uint16_t *tss10);
// Duration and workout name ("" for free rides) of one summary.csv row, to tell which plan
// workouts were ridden. False for the header, a malformed row or a name longer than size - 1.
bool csv_summary_workout(const char *line, uint32_t *duration_s, char *workout, size_t size);
// All best-effort rows for one ride (skips durations longer than the ride).
size_t csv_format_bests(char *buf, size_t size, const char *ride_id, const int16_t bests[METRICS_N_BESTS]);

// True if a summary/bests file already has a row for this ride (crash recovery
// can write a ride's outputs more than once).
bool csv_has_ride(const char *path, const char *ride_id);

// One summary/bests line with a ride's new id (a ride recorded before the clock was set).
// new_start: also replace the start time (summary rows); 0 for bests rows. Lines of other
// rides are copied unchanged. False if out is too small or the row is malformed.
bool csv_redate_line(char *out, size_t size, const char *line, const char *old_id, const char *new_id,
                     uint32_t new_start);

// "2026-10-26T12:00:00Z"; buf >= 21 bytes
void csv_iso_utc(char *buf, uint32_t t);
// "2026-10-26_1200" (ride id / file stem, from local time); buf >= 16 bytes
void csv_ride_id(char *buf, uint32_t t);
