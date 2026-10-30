#include <string.h>
#include "ghost.h"
#include "test.h"

static ghost_t g;

static void ride(int seconds, int kmh100)
{
    for (int i = 0; i < seconds; i++) {
        ride_sample_t s = {.speed_kmh100 = kmh100};
        ghost_add(&g, &s);
    }
}

static void test_distance_and_time(void)
{
    ghost_begin(&g);
    CHECK_EQ(ghost_dist_m(&g, 10), 0);
    CHECK_EQ(ghost_time_at(&g, 1), -1);
    ride(100, 3600);  // 36 km/h = 10 m/s
    ride(10, -1);     // unknown speed: no distance
    ride(100, 1800);  // 5 m/s
    CHECK_EQ(ghost_dist_m(&g, 0), 0);
    CHECK_EQ(ghost_dist_m(&g, 1), 10);
    CHECK_EQ(ghost_dist_m(&g, 100), 1000);
    CHECK_EQ(ghost_dist_m(&g, 110), 1000);
    CHECK_EQ(ghost_dist_m(&g, 210), 1500);
    CHECK_EQ(ghost_dist_m(&g, 9999), 1500);  // clamped to the end
    CHECK_EQ(ghost_time_at(&g, 0), 0);
    CHECK_EQ(ghost_time_at(&g, 10), 1);
    CHECK_EQ(ghost_time_at(&g, 15), 2);
    CHECK_EQ(ghost_time_at(&g, 1000), 100);
    CHECK_EQ(ghost_time_at(&g, 1005), 111);
    CHECK_EQ(ghost_time_at(&g, 1500), 210);
    CHECK_EQ(ghost_time_at(&g, 1501), -1);
}

static void test_full(void)
{
    ghost_begin(&g);
    ride(GHOST_MAX_S, 3600);
    ride_sample_t s = {.speed_kmh100 = 3600};
    CHECK(!ghost_add(&g, &s));
    CHECK_EQ(g.n, GHOST_MAX_S);
}

static void test_summary_row(void)
{
    ghost_summary_t r;
    CHECK(!ghost_summary_row("ride_id,start_utc,duration_s,moving_s,workout,ftp_w,avg_w,np_w,if,tss,kj,avg_hr,max_hr,"
                             "avg_cad,distance_km,fw,weight_kg,user_max_hr\n", &r));
    CHECK(ghost_summary_row("2026-10-30_1802,2026-10-30T17:02:00Z,1834,1820,\"Sample climb (made up)\",250,231,240,"
                            "0.960,28.1,424,151,171,88.2,8.123,0.1.0,75,185\r\n", &r));
    CHECK(strcmp(r.ride_id, "2026-10-30_1802") == 0);
    CHECK(strcmp(r.workout, "Sample climb (made up)") == 0);
    CHECK_EQ(r.duration_s, 1834);
    CHECK_EQ(r.distance_m, 8123);
    CHECK(ghost_summary_row("2026-10-30_1802,x,60,60,\"\",250,1,1,0.1,0.1,1,0,0,0.0,0.5,fw,75,185", &r));
    CHECK_EQ(r.distance_m, 500);
    CHECK(!ghost_summary_row("2026-10-30_1802,x,60", &r));
}

int main(void)
{
    test_distance_and_time();
    test_full();
    test_summary_row();
    return test_summary("ghost");
}
