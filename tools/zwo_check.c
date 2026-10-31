// zwo_check: runs the firmware's own .zwo parser (components/workout) on a file
// and prints what the device would see, as one JSON line. Exit 0 = the device
// accepts the file, 1 = it doesn't, 2 = usage or I/O error.
// Used by tools/mcp_server.py before it uploads a workout; built by test/host too.
// Build: cc -O2 -std=c11 -Icomponents/workout/include tools/zwo_check.c components/workout/zwo.c
//        components/workout/workout.c -lm -o build/zwo_check
#include <stdio.h>
#include <stdlib.h>
#include "workout.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: zwo_check <file.zwo | ->\n");
        return 2;
    }
    FILE *f = argv[1][0] == '-' && !argv[1][1] ? stdin : fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    static char buf[ZWO_MAX_BYTES + 1];
    size_t len = fread(buf, 1, sizeof(buf), f);
    if (f != stdin) {
        fclose(f);
    }
    if (len > ZWO_MAX_BYTES) {
        printf("{\"ok\":false,\"error\":\"larger than %d bytes\"}\n", ZWO_MAX_BYTES);
        return 1;
    }
    static workout_t w;
    if (!zwo_parse(buf, len, &w)) {
        printf("{\"ok\":false,\"error\":\"no usable segments\"}\n");
        return 1;
    }
    workout_est_t e;
    workout_estimate(&w, &e);
    unsigned max_goal_pm = 0;
    for (int i = 0; i < w.n_goals; i++) {
        if (w.goals[i].max_pm > max_goal_pm) {
            max_goal_pm = w.goals[i].max_pm;
        }
    }
    // name is printed as JSON: escape quotes, backslashes and control characters
    printf("{\"ok\":true,\"name\":\"");
    for (const char *c = w.name; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (u == '"' || u == '\\') {
            printf("\\%c", u);
        } else if (u < 0x20) {
            printf("\\u%04x", u);
        } else {
            putchar(u);
        }
    }
    printf("\",\"segments\":%u,\"goals\":%u,\"ramp_test\":%s,\"total_s\":%lu,\"avg_pm\":%u,\"max_pm\":%u,"
           "\"max_goal_pm\":%u,\"if_pm\":%u,\"tss10\":%u}\n",
           w.n, w.n_goals, w.ramp_test ? "true" : "false", (unsigned long)e.total_s, e.avg_pm, e.max_pm,
           max_goal_pm, e.if_pm, e.tss10);
    return 0;
}
