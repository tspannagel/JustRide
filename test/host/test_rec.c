#include <stdio.h>
#include <stdlib.h>
#include "rec_csv.h"
#include "rec_fit.h"
#include "test.h"

#define T0 1793016000u  // 2026-10-26T12:00:00Z

static void test_time(void)
{
    char buf[21];
    csv_iso_utc(buf, T0);
    CHECK(strcmp(buf, "2026-10-26T12:00:00Z") == 0);
    csv_iso_utc(buf, 951782400); // leap day 2000-02-29
    CHECK(strcmp(buf, "2000-02-29T00:00:00Z") == 0);
    csv_ride_id(buf, T0 + 5 * 60);
    CHECK(strcmp(buf, "2026-10-26_1205") == 0);
}

static void test_row_roundtrip(void)
{
    ride_sample_t s = {.t_s = 42, .ts_utc = T0 + 42, .power_w = 250, .cadence_rpm10 = 905, .speed_kmh100 = 3012,
                       .hr_bpm = 151, .target_w = 250, .grade = -125, .gear = 12, .interval_idx = 3,
                       .mode = MODE_ERG, .lap = 1};
    char row[CSV_ROW_MAX];
    size_t n = csv_format_row(row, sizeof(row), &s);
    CHECK(n > 0);
    CHECK(strcmp(row, "42,2026-10-26T12:00:42Z,250,90.5,30.12,151,250,-1.25,12,3,erg,1\n") == 0);

    ride_sample_t p;
    CHECK(csv_parse_row(row, &p));
    CHECK(memcmp(&s, &p, sizeof(s)) == 0);

    // Unknowns are empty and come back as -1
    s.power_w = -1;
    s.cadence_rpm10 = -1;
    s.speed_kmh100 = -1;
    s.hr_bpm = -1;
    s.mode = MODE_FREE;
    csv_format_row(row, sizeof(row), &s);
    CHECK(strcmp(row, "42,2026-10-26T12:00:42Z,,,,,250,-1.25,12,3,free,1\n") == 0);
    CHECK(csv_parse_row(row, &p));
    CHECK(memcmp(&s, &p, sizeof(s)) == 0);

    CHECK(!csv_parse_row(CSV_RIDE_HEADER, &p));
    CHECK(!csv_parse_row("42,2026-10-26T12:00:42Z,250,90", &p)); // torn last line after a crash
    CHECK(!csv_parse_row("", &p));
    CHECK_EQ(csv_format_row(row, 20, &s), 0); // too small: refuse
}

static void test_summary_and_bests(void)
{
    metrics_summary_t m = {.duration_s = 3600, .moving_s = 3590, .avg_w = 200, .np_w = 210, .if_1000 = 840,
                           .tss10 = 706, .kj = 720, .avg_hr = 140, .max_hr = 170, .avg_cad10 = 887,
                           .distance_m = 32050};
    char buf[512];
    CHECK(csv_format_summary(buf, sizeof(buf), "2026-10-26_1200", T0, "Sweet \"Spot\", 3x10", 250, &m, "0.1.0") > 0);
    CHECK(strcmp(buf, "2026-10-26_1200,2026-10-26T12:00:00Z,3600,3590,\"Sweet Spot, 3x10\",250,200,210,0.840,70.6,"
                      "720,140,170,88.7,32.050,0.1.0\n") == 0);

    int16_t b[METRICS_N_BESTS] = {900, 700, 600, 400, 350, 320, 290, -1, -1, -1, -1};
    CHECK(csv_format_bests(buf, sizeof(buf), "r1", b) > 0);
    CHECK(strncmp(buf, "r1,1,900\nr1,5,700\n", 18) == 0);
    CHECK(strstr(buf, ",600,") == NULL);
}

static bool to_file(void *ctx, const void *d, size_t n) { return fwrite(d, 1, n, ctx) == n; }

static uint8_t mem[1 << 20];
static size_t mem_len;
static bool to_mem(void *ctx, const void *d, size_t n)
{
    (void)ctx;
    if (mem_len + n > sizeof(mem)) {
        return false;
    }
    memcpy(mem + mem_len, d, n);
    mem_len += n;
    return true;
}

static void test_fit(void)
{
    CHECK_EQ(fit_crc16(0, "123456789", 9), 0xBB3D); // CRC-16/ARC check value

    size_t n = 1200;
    ride_sample_t *s = calloc(n, sizeof(*s));
    for (size_t i = 0; i < n; i++) {
        s[i].t_s = (uint32_t)i;
        s[i].ts_utc = T0 + (uint32_t)i + (i >= 600 ? 60 : 0); // 60 s pause at 10 min
        s[i].power_w = i < 600 ? 200 : 300;
        s[i].cadence_rpm10 = 900;
        s[i].speed_kmh100 = 3600;
        s[i].hr_bpm = i < 10 ? -1 : 140;
        s[i].lap = i < 600 ? 0 : 1;
    }
    fit_meta_t meta = {.ftp_w = 250, .sw_version = 10};
    mem_len = 0;
    CHECK(fit_write_activity(to_mem, NULL, s, n, &meta));
    CHECK(mem_len > 14 + 2);
    CHECK(memcmp(mem + 8, ".FIT", 4) == 0);
    uint32_t data_size = mem[4] | mem[5] << 8 | mem[6] << 16 | (uint32_t)mem[7] << 24;
    CHECK_EQ(data_size, mem_len - 14 - 2);
    CHECK_EQ(fit_crc16(0, mem, 12), mem[12] | mem[13] << 8);
    CHECK_EQ(fit_crc16(0, mem, mem_len), 0); // whole file incl. CRC checks to 0

    // Optional: dump for external validation (fitdecode / FitCSVTool)
    const char *path = getenv("FIT_OUT");
    if (path) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fit_write_activity(to_file, f, s, n, &meta);
            fclose(f);
        }
    }
    CHECK(!fit_write_activity(to_mem, NULL, s, 0, &meta));
    free(s);
}

int main(void)
{
    test_time();
    test_row_roundtrip();
    test_summary_and_bests();
    test_fit();
    return test_summary("rec");
}
