#include "test.h"
#include "vshift.h"

int main(void)
{
    gear_table_t g;
    vshift_default(&g);
    CHECK_EQ(g.n, 24);
    CHECK_EQ(g.ratio100[0], 75);
    CHECK_EQ(g.ratio100[23], 550);
    for (int i = 1; i < g.n; i++) {
        CHECK(g.ratio100[i] > g.ratio100[i - 1]);
    }

    int n = vshift_neutral_gear(&g);
    CHECK(g.ratio100[n] >= 190 && g.ratio100[n] <= 210);

    // 0.01 % grade and 0.0001 Crr are the same unit (1e-4), so they add directly.
    // Real gear (k = 1): grade unchanged.
    gear_table_t one = {.n = 1, .ratio100 = {200}, .real_ratio100 = 200};
    CHECK_EQ(vshift_grade(&one, 0, 300, 40), 300);
    // k = 2 at 3 %: (300 + 40) * 2 - 40 = 640 (6.40 %)
    gear_table_t two = {.n = 1, .ratio100 = {400}, .real_ratio100 = 200};
    CHECK_EQ(vshift_grade(&two, 0, 300, 40), 640);
    // k = 0.5 at 0 %: (0 + 40) * 0.5 - 40 = -20
    gear_table_t half = {.n = 1, .ratio100 = {100}, .real_ratio100 = 200};
    CHECK_EQ(vshift_grade(&half, 0, 0, 40), -20);

    // Clamps: steep climb in the biggest gear, steep descent
    CHECK_EQ(vshift_grade(&g, 23, 1500, 40), VSHIFT_GRADE_MAX);
    CHECK_EQ(vshift_grade(&g, 23, -1500, 40), VSHIFT_GRADE_MIN);
    // Out-of-range gear index is clamped, not read out of bounds
    CHECK_EQ(vshift_grade(&g, 99, 0, 40), vshift_grade(&g, 23, 0, 40));
    CHECK_EQ(vshift_grade(&g, -5, 0, 40), vshift_grade(&g, 0, 0, 40));
    return test_summary("vshift");
}
