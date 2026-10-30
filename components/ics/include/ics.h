// One calendar event as an iCalendar file (RFC 5545), for "Add to calendar".
// Pure C. Times are UTC; the calendar app shows them in local time.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *uid;          // same uid = same event: adding it again updates it
    uint32_t stamp_utc;       // when the file was made
    uint32_t start_utc;
    uint32_t duration_s;
    const char *summary;
    const char *description;  // may contain newlines
    uint16_t alarm_min;       // reminder before the start, 0 = none
} ics_event_t;

// Writes the whole file (CRLF line ends, folded at 75 bytes, text escaped).
// Returns its length, 0 if buf is too small. buf is always NUL-terminated.
size_t ics_write(char *buf, size_t size, const ics_event_t *e);
