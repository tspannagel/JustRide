#include <stdio.h>
#include <stdlib.h>
#include "test.h"
#include "workout.h"

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(*len);
    *len = fread(buf, 1, *len, f);
    fclose(f);
    return buf;
}

static workout_t w;

static void test_sweet_spot(void)
{
    size_t len;
    char *xml = read_file(TOOLS_DIR "/workouts/sweet_spot_3x10.zwo", &len);
    CHECK(xml != NULL);
    if (!xml) {
        return;
    }
    CHECK(zwo_parse(xml, len, &w));
    free(xml);
    CHECK(strcmp(w.name, "Sweet Spot 3x10") == 0);
    CHECK_EQ(w.n, 8); // warmup, 3 x (on, off), cooldown
    CHECK_EQ(w.total_s, 600 + 3 * 900 + 300);
    CHECK_EQ(w.seg[0].p_start, 450);
    CHECK_EQ(w.seg[0].p_end, 750);
    CHECK_EQ(w.seg[1].p_start, 900);
    CHECK_EQ(w.seg[1].cadence, 90);
    CHECK_EQ(w.seg[2].p_start, 550);
    CHECK_EQ(w.seg[2].cadence, 85);
    CHECK_EQ(w.seg[7].p_start, 600);
    CHECK_EQ(w.seg[7].p_end, 400);

    workout_pos_t pos;
    workout_at(&w, 0, &pos);
    CHECK_EQ(pos.idx, 0);
    CHECK_EQ(pos.target_pm, 450);
    CHECK_EQ(pos.seg_left_s, 600);
    workout_at(&w, 300, &pos); // halfway through the warmup ramp
    CHECK_EQ(pos.target_pm, 600);
    workout_at(&w, 600, &pos);
    CHECK_EQ(pos.idx, 1);
    CHECK_EQ(pos.target_pm, 900);
    CHECK(!pos.done);
    workout_at(&w, w.total_s, &pos);
    CHECK(pos.done);
    CHECK_EQ(workout_seg_start(&w, 2), 1200);
}

static void test_endurance_comment_and_free(void)
{
    size_t len;
    char *xml = read_file(TOOLS_DIR "/workouts/endurance_60.zwo", &len);
    CHECK(xml != NULL);
    if (!xml) {
        return;
    }
    CHECK(zwo_parse(xml, len, &w));
    free(xml);
    CHECK_EQ(w.n, 3); // the SteadyState inside the comment is ignored
    CHECK_EQ(w.seg[1].p_start, 680);
    CHECK_EQ(w.seg[1].cadence, 88);
    CHECK_EQ(w.seg[2].kind, SEG_FREE);
    CHECK_EQ(w.seg[2].p_start, 0);
}

static void test_garbage_and_limits(void)
{
    const char *bad = "<workout_file><workout></workout></workout_file>";
    CHECK(!zwo_parse(bad, strlen(bad), &w));
    CHECK(!zwo_parse("<", 1, &w));
    CHECK(!zwo_parse("<!-- unterminated", 17, &w));

    const char *big = "<SteadyState Duration='999999' Power='9.5'/><SteadyState Duration='-5' Power='1'/>";
    CHECK(zwo_parse(big, strlen(big), &w));
    CHECK_EQ(w.n, 1);
    CHECK_EQ(w.seg[0].dur_s, 36000);
    CHECK_EQ(w.seg[0].p_start, 3000);

    // Truncated in the middle of an attribute
    const char *cut = "<SteadyState Duration=\"60\" Power=\"0.";
    CHECK(!zwo_parse(cut, strlen(cut), &w));

    // Non-numbers must not reach the integer casts (UBSan float-cast-overflow)
    const char *nan = "<SteadyState Duration='nan' Power='1'/><IntervalsT Repeat='nan' OnDuration='inf' "
                      "OffDuration='60' OnPower='nan' OffPower='0.5'/>";
    CHECK(zwo_parse(nan, strlen(nan), &w));
    CHECK_EQ(w.n, 1); // only the 60 s off interval is usable
    CHECK_EQ(w.seg[0].dur_s, 60);
    CHECK_EQ(w.seg[0].p_start, 500);
}

static void test_estimate(void)
{
    workout_t hour = {.n = 1, .total_s = 3600, .seg = {{.dur_s = 3600, .p_start = 1000, .p_end = 1000}}};
    workout_est_t e;
    workout_estimate(&hour, &e);
    CHECK_EQ(e.total_s, 3600);
    CHECK_EQ(e.avg_pm, 1000);
    CHECK_EQ(e.if_pm, 1000);
    CHECK_EQ(e.tss10, 1000);  // 1 h at FTP = 100 TSS

    // 30 min free (counted as 50 %) + 30 min ramp 0.5 -> 1.0
    workout_t mix = {.n = 2, .total_s = 3600,
                     .seg = {{.dur_s = 1800, .kind = SEG_FREE}, {.dur_s = 1800, .p_start = 500, .p_end = 1000}}};
    workout_estimate(&mix, &e);
    CHECK_EQ(e.avg_pm, 625);
    CHECK(e.max_pm >= 999 && e.max_pm <= 1000);
    CHECK(e.if_pm > e.avg_pm && e.if_pm < 800);  // 4th-power mean weights the hard part
    CHECK_EQ(workout_profile_at(&mix, 0), 500);
    CHECK_EQ(workout_profile_at(&mix, 2700), 750);
    CHECK_EQ(workout_profile_at(&mix, 3600), 0);

    workout_t empty = {0};
    workout_estimate(&empty, &e);
    CHECK_EQ(e.tss10, 0);
}

static void test_adaptive_goals(void)
{
    const char *xml = "<workout_file><name>Adaptive</name><workout>"
                      "<Warmup Duration=\"300\" PowerLow=\"0.5\" PowerHigh=\"0.65\"/>"
                      "<SteadyState Duration=\"1800\" Power=\"0.65\" JrHrLow=\"150\" JrHrHigh=\"140\" JrCadLow=\"85\" JrCadHigh=\"95\"/>"
                      "<SteadyState Duration=\"600\" Power=\"0.7\" JrAvg=\"1\" JrMin=\"0.5\" JrMax=\"0.8\"/>"
                      "<SteadyState Duration=\"600\" Power=\"0.7\" JrHrLow=\"140\"/>"  // half a range: no goal
                      "</workout></workout_file>";
    CHECK(zwo_parse(xml, strlen(xml), &w));
    CHECK_EQ(w.n, 4);
    CHECK_EQ(w.n_goals, 2);
    CHECK(workout_goal(&w, 0) == NULL);
    const goal_t *g = workout_goal(&w, 1);
    CHECK(g != NULL);
    if (g) {
        CHECK_EQ(g->hr_lo, 140);  // swapped into order
        CHECK_EQ(g->hr_hi, 150);
        CHECK_EQ(g->cad_lo, 85);
        CHECK_EQ(g->cad_hi, 95);
        CHECK_EQ(g->min_pm, 455);  // default: -30 %
        CHECK_EQ(g->max_pm, 747);  // default: +15 %
        CHECK(!g->hold_avg);
    }
    g = workout_goal(&w, 2);
    CHECK(g != NULL);
    if (g) {
        CHECK(g->hold_avg);
        CHECK_EQ(g->min_pm, 500);
        CHECK_EQ(g->max_pm, 800);
    }
    CHECK(workout_goal(&w, 3) == NULL);
    workout_pos_t pos;
    workout_at(&w, 400, &pos);
    CHECK_EQ(pos.goal, 1);

    // The table is bounded: more adaptive segments than slots stay fixed segments
    workout_t many = {0};
    goal_t avg = {.hold_avg = true};
    for (int i = 0; i < WORKOUT_MAX_GOALS + 3; i++) {
        CHECK_EQ(workout_add_goal(&many, 600, avg), i < WORKOUT_MAX_GOALS ? i + 1 : 0);
    }
    // Bounds never below 30 % FTP
    goal_t low = {.hold_avg = true};
    workout_t one = {0};
    workout_add_goal(&one, 200, low);
    CHECK_EQ(one.goals[0].min_pm, 300);
    CHECK_EQ(one.goals[0].max_pm, 300);
}

int main(void)
{
    test_sweet_spot();
    test_endurance_comment_and_free();
    test_garbage_and_limits();
    test_estimate();
    test_adaptive_goals();
    return test_summary("workout");
}
