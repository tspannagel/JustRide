#include <stdio.h>
#include <string.h>
#include "route.h"
#include "test.h"

static route_t r;

// Parse a whole text, line by line, as the device reads the file
static bool parse(const char *text)
{
    route_parser_t p;
    route_parse_begin(&p, &r);
    char line[128];
    const char *s = text;
    while (*s) {
        size_t n = strcspn(s, "\n");
        snprintf(line, sizeof(line), "%.*s", (int)n, s);
        route_parse_line(&p, line);
        s += n + (s[n] == '\n');
    }
    return route_parse_end(&p);
}

static void test_parse(void)
{
    CHECK(parse("JustRide route 1\r\nname=Test climb\r\nstep_m=10\r\n10000\r\n10050\r\n10100\r\n"));
    CHECK(strcmp(r.name, "Test climb") == 0);
    CHECK_EQ(r.step_m, 10);
    CHECK_EQ(r.n, 3);
    CHECK_EQ(route_length_m(&r), 20);

    CHECK(!parse(""));
    CHECK(!parse("JustRide route 2\nstep_m=10\n0\n0\n"));       // unknown version
    CHECK(!parse("JustRide route 1\n0\n0\n"));                  // points before step_m
    CHECK(!parse("JustRide route 1\nstep_m=1\n0\n0\n"));        // step too small
    CHECK(!parse("JustRide route 1\nstep_m=10\n0\n"));          // one point
    CHECK(!parse("JustRide route 1\nstep_m=10\n0\n99999999\n")); // 1000 km high
    CHECK(!parse("JustRide route 1\nstep_m=10\n0\n12abc\n"));
    CHECK(!parse("JustRide route 1\nstep_m=10\n0\nlat=1\n"));
    CHECK(!parse("JustRide route 1\nstep_m=10\n0\n0\nstep_m=20\n"));  // step after points
    CHECK(parse("JustRide route 1\nname=\x01x\nstep_m=10\n0\n0\n"));
    CHECK_EQ(r.name[0], ' ');  // control characters replaced
}

static void test_too_many_points(void)
{
    static char text[(ROUTE_MAX_PTS + 10) * 3];
    char *t = text + sprintf(text, "JustRide route 1\nstep_m=10\n");
    for (int i = 0; i <= ROUTE_MAX_PTS; i++) {
        t += sprintf(t, "0\n");
    }
    CHECK(!parse(text));
}

// A 1 km climb at 8 % after 500 m of flat, then 500 m of flat
static void climb(void)
{
    route_parser_t p;
    route_parse_begin(&p, &r);
    route_parse_line(&p, "JustRide route 1");
    route_parse_line(&p, "step_m=10");
    char line[16];
    for (int d = 0; d <= 2000; d += 10) {
        int ele = d < 500 ? 0 : d < 1500 ? (d - 500) * 8 : 8000;  // cm
        snprintf(line, sizeof(line), "%d", ele);
        route_parse_line(&p, line);
    }
    CHECK(route_parse_end(&p));
}

static void test_grade_and_info(void)
{
    climb();
    CHECK_EQ(route_length_m(&r), 2000);
    CHECK_EQ(route_grade(&r, 200), 0);
    CHECK_EQ(route_grade(&r, 1000), 800);
    CHECK_EQ(route_grade(&r, 500), 400);   // half of the window on the climb
    CHECK_EQ(route_grade(&r, 1900), 0);
    CHECK_EQ(route_grade(&r, 5000), 0);    // past the end: last window
    CHECK_EQ(route_ele_cm(&r, 1000), 4000);
    CHECK_EQ(route_ele_cm(&r, 1005), 4040);
    CHECK_EQ(route_ele_cm(&r, 99999), 8000);
    route_info_t i;
    route_info(&r, &i);
    CHECK_EQ(i.length_m, 2000);
    CHECK_EQ(i.climb_m, 80);
    CHECK_EQ(i.max_cm, 8000);
    CHECK_EQ(i.max_grade, 800);
}

// One bad point (a 50 m spike) is spread over the window and clamped
static void test_spike_is_clamped(void)
{
    CHECK(parse("JustRide route 1\nstep_m=10\n0\n0\n0\n0\n0\n0\n5000\n0\n0\n0\n0\n0\n0\n"));
    for (uint32_t d = 0; d <= route_length_m(&r); d += 5) {
        int g = route_grade(&r, d);
        CHECK(g >= -ROUTE_GRADE_MAX && g <= ROUTE_GRADE_MAX);
    }
}

int main(void)
{
    test_parse();
    test_too_many_points();
    test_grade_and_info();
    test_spike_is_clamped();
    return test_summary("route");
}
