// Local time from a POSIX TZ rule ("CET-1CEST,M3.5.0,M10.5.0/3"). Pure C.
// The device clock and libc stay in UTC (bsp_rtc_get relies on it); only ride
// names and the FIT local timestamp use this.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TZ_MAX_LEN 48  // bytes incl. NUL

typedef struct {
    const char *name;   // shown on the device
    const char *posix;
} tz_preset_t;

extern const tz_preset_t tz_presets[];
extern const int tz_n_presets;

// Supported: std offset [dst [offset] [,start[/time],end[/time]]] with Mm.w.d,
// Jn and n dates; DST without a rule uses the US rule, like glibc.
bool tz_valid(const char *posix);
// Seconds to add to UTC for local time; 0 if the rule is invalid.
int32_t tz_offset(const char *posix, uint32_t utc);
// Index into tz_presets, -1 if the rule isn't one of them.
int tz_preset_index(const char *posix);
