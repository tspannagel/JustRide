#include <stdio.h>
#include <stdlib.h>
#include "ics.h"
#include "test.h"

#define T0 1793016000u  // 2026-10-26T12:00:00Z

static char buf[4096];

static void test_event(void)
{
    ics_event_t e = {.uid = "1793016000-sweet spot@justride", .stamp_utc = T0 - 3600, .start_utc = T0,
                     .duration_s = 3720, .summary = "Sweet Spot, 2x20; hard",
                     .description = "62 min - TSS 71\nback\\slash", .alarm_min = 15};
    size_t n = ics_write(buf, sizeof(buf), &e);
    CHECK(n > 0);
    CHECK_EQ(n, strlen(buf));
    CHECK(strncmp(buf, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\n", 30) == 0);
    CHECK(strstr(buf, "\r\nDTSTART:20261026T120000Z\r\n"));
    CHECK(strstr(buf, "\r\nDTEND:20261026T130200Z\r\n"));
    CHECK(strstr(buf, "\r\nDTSTAMP:20261026T110000Z\r\n"));
    CHECK(strstr(buf, "\r\nSUMMARY:Sweet Spot\\, 2x20\\; hard\r\n"));
    CHECK(strstr(buf, "\r\nDESCRIPTION:62 min - TSS 71\\nback\\\\slash\r\n"));
    CHECK(strstr(buf, "\r\nTRIGGER:-PT15M\r\n"));
    CHECK(strcmp(buf + n - 15, "END:VCALENDAR\r\n") == 0);
    // Every line ends in CRLF; no bare LF
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
            CHECK(i > 0 && buf[i - 1] == '\r');
        }
    }

    // Optional: dump for external validation (python icalendar)
    const char *path = getenv("ICS_OUT");
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }

    e.alarm_min = 0;
    ics_write(buf, sizeof(buf), &e);
    CHECK(!strstr(buf, "VALARM"));
}

static void test_folding(void)
{
    char desc[400];
    // 2-byte UTF-8 characters, so a fold in the wrong place would split one
    for (int i = 0; i < 99; i++) {
        memcpy(desc + i * 2, "\xC3\xA9", 2);  // é
    }
    desc[198] = 0;
    ics_event_t e = {.uid = "x", .start_utc = T0, .duration_s = 60, .summary = "s", .description = desc};
    size_t n = ics_write(buf, sizeof(buf), &e);
    CHECK(n > 0);
    // Lines are at most 75 bytes and continuation lines start with a space
    size_t col = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r') {
            CHECK_EQ(buf[i + 1], '\n');
            CHECK(col <= 75);
            if (i + 2 < n && buf[i + 2] == ' ') {
                CHECK(((unsigned char)buf[i + 3] & 0xC0) != 0x80);  // not inside a character
            }
            col = 0;
            i++;
        } else {
            col++;
        }
    }
    // Unfolding gives back the original value
    char unfolded[4096];
    size_t u = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' && buf[i + 2] == ' ') {
            i += 2;
            continue;
        }
        unfolded[u++] = buf[i];
    }
    unfolded[u] = 0;
    char want[420];
    snprintf(want, sizeof(want), "\r\nDESCRIPTION:%s\r\n", desc);
    CHECK(strstr(unfolded, want));
}

static void test_too_small(void)
{
    ics_event_t e = {.uid = "x", .start_utc = T0, .duration_s = 60, .summary = "s", .description = "d"};
    size_t n = ics_write(buf, sizeof(buf), &e);
    CHECK(n > 0);
    char small[64];
    CHECK_EQ(ics_write(small, sizeof(small), &e), 0);
    CHECK_EQ(small[0], 0);
    CHECK_EQ(ics_write(buf, n, &e), 0);         // no room for the NUL
    CHECK_EQ(ics_write(buf, n + 1, &e), n);     // exactly enough
}

int main(void)
{
    test_event();
    test_folding();
    test_too_small();
    return test_summary("ics");
}
