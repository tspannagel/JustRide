// tz_offset against glibc's own POSIX TZ implementation, hourly over several
// years, plus the minutes around every transition.
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <time.h>
#include "test.h"
#include "tz.h"

static long libc_offset(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_gmtoff;
}

static void compare(const char *posix)
{
    CHECK(tz_valid(posix));
    setenv("TZ", posix, 1);
    tzset();
    const time_t from = 1704067200;  // 2024-01-01
    const time_t to = 1830297600;    // 2028-01-01
    int bad = 0;
    long prev = libc_offset(from);
    for (time_t t = from; t < to && bad < 3; t += 900) {
        long want = libc_offset(t);
        if (want != prev) {
            // Transition in the last 15 min: check each second around it
            for (time_t s = t - 900; s <= t && bad < 3; s++) {
                if (tz_offset(posix, (uint32_t)s) != libc_offset(s)) {
                    printf("%s at %ld: %d, libc %ld\n", posix, (long)s, (int)tz_offset(posix, (uint32_t)s),
                           libc_offset(s));
                    bad++;
                }
            }
            prev = want;
        } else if (tz_offset(posix, (uint32_t)t) != want) {
            printf("%s at %ld: %d, libc %ld\n", posix, (long)t, (int)tz_offset(posix, (uint32_t)t), want);
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
}

int main(void)
{
    for (int i = 0; i < tz_n_presets; i++) {
        compare(tz_presets[i].posix);
    }
    compare("EST5EDT");                   // DST without a rule: US default
    compare("XXX3YYY,J60/1,J300/1");      // Julian days without Feb 29
    compare("XXX-2YYY-3,59/-1,300/26");   // zero-based days, negative and > 24 h times
    compare("<+0545>-5:45");

    CHECK_EQ(tz_offset("CET-1CEST,M3.5.0,M10.5.0/3", 1793016000u), 3600);  // 2026-10-26: winter
    CHECK_EQ(tz_offset("CET-1CEST,M3.5.0,M10.5.0/3", 1782475200u), 7200);  // 2026-06-26: summer
    CHECK_EQ(tz_preset_index("UTC0"), 0);
    CHECK_EQ(tz_preset_index("nope"), -1);

    const char *bad[] = {"", "UT0", "CET", "CET-1CEST,M3.5.0", "CET-1CEST,M13.5.0,M10.5.0", "CET-25",
                         "CET-1CEST,M3.5.0,M10.5.0/3x", "<AB>1", "CET-1 CEST", NULL,
                         "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!tz_valid(bad[i]));
        CHECK_EQ(tz_offset(bad[i], 1793016000u), 0);
    }
    return test_summary("tz");
}
