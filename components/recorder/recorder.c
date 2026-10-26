#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rec_csv.h"
#include "rec_fit.h"

static const char *TAG = "rec";

#define JOURNAL RIDES_DIR "/.journal"
#define FLUSH_EVERY_S 10
#define PENDING_SIZE (FLUSH_EVERY_S * 2 * CSV_ROW_MAX)

static ride_sample_t *s_buf;  // PSRAM, RECORDER_MAX_S samples
static size_t s_n;
static FILE *s_csv;
static char s_pending[PENDING_SIZE];
static size_t s_pending_len;
static bool s_write_error;

static struct {
    char id[16];
    uint32_t start_utc;
    uint16_t ftp_w;
    char workout[48];
} s_meta;

void recorder_init(void)
{
    s_buf = heap_caps_malloc(RECORDER_MAX_S * sizeof(ride_sample_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the ride buffer");
    }
}

static void path(char *out, size_t size, const char *ext)
{
    snprintf(out, size, RIDES_DIR "/%s.%s", s_meta.id, ext);
}

static void flush(void)
{
    if (!s_csv || s_pending_len == 0) {
        return;
    }
    bool ok = fwrite(s_pending, 1, s_pending_len, s_csv) == s_pending_len;
    ok = ok && fflush(s_csv) == 0 && fsync(fileno(s_csv)) == 0;
    if (!ok && !s_write_error) {
        ESP_LOGE(TAG, "CSV write failed (SD removed?); ride kept in memory");
    }
    s_write_error |= !ok;
    s_pending_len = 0;
}

bool recorder_start(uint32_t start_utc, const char *workout, uint16_t ftp_w)
{
    if (!s_buf) {
        return false;
    }
    mkdir(RIDES_DIR, 0775);
    memset(&s_meta, 0, sizeof(s_meta));
    csv_ride_id(s_meta.id, start_utc);
    s_meta.start_utc = start_utc;
    s_meta.ftp_w = ftp_w;
    snprintf(s_meta.workout, sizeof(s_meta.workout), "%s", workout ? workout : "");
    s_n = 0;
    s_pending_len = 0;
    s_write_error = false;

    char p[64];
    path(p, sizeof(p), "csv");
    s_csv = fopen(p, "w");
    if (s_csv) {
        fputs(CSV_RIDE_HEADER, s_csv);
    }
    FILE *j = fopen(JOURNAL, "w");
    if (j) {
        fprintf(j, "%s\n%lu\n%u\n%s\n", s_meta.id, (unsigned long)start_utc, ftp_w, s_meta.workout);
        fclose(j);
    }
    if (!s_csv || !j) {
        ESP_LOGW(TAG, "SD not writable; recording to memory only");
    }
    return true;
}

void recorder_add(const ride_sample_t *s)
{
    if (!s_buf || s_n >= RECORDER_MAX_S) {
        return;
    }
    s_buf[s_n++] = *s;
    size_t len = csv_format_row(s_pending + s_pending_len, sizeof(s_pending) - s_pending_len, s);
    s_pending_len += len;
    if (s_n % FLUSH_EVERY_S == 0 || sizeof(s_pending) - s_pending_len < CSV_ROW_MAX) {
        flush();
    }
}

static bool fit_sink(void *ctx, const void *d, size_t n) { return fwrite(d, 1, n, ctx) == n; }

static bool append(const char *file, const char *header, const char *text)
{
    struct stat st;
    bool fresh = stat(file, &st) != 0;
    FILE *f = fopen(file, "a");
    if (!f) {
        return false;
    }
    if (fresh) {
        fputs(header, f);
    }
    bool ok = fputs(text, f) >= 0;
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    return fclose(f) == 0 && ok;
}

static void discard(void)
{
    char p[64];
    path(p, sizeof(p), "csv");
    remove(p);
    remove(JOURNAL);
}

// Shared by a normal finish and crash recovery: s_buf/s_n/s_meta are filled.
static bool write_outputs(void)
{
    if (s_n < RECORDER_MIN_S) {
        ESP_LOGI(TAG, "ride shorter than %d s, discarded", RECORDER_MIN_S);
        discard();
        return false;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    char p[64];
    path(p, sizeof(p), "fit");
    FILE *f = fopen(p, "wb");
    fit_meta_t meta = {.ftp_w = s_meta.ftp_w, .sw_version = 100};
    bool ok = f && fit_write_activity(fit_sink, f, s_buf, s_n, &meta);
    if (f) {
        ok = fclose(f) == 0 && ok;
    }

    metrics_t m;
    metrics_init(&m);
    for (size_t i = 0; i < s_n; i++) {
        metrics_add(&m, &s_buf[i]);
    }
    metrics_summary_t sum;
    metrics_summary(&m, s_meta.ftp_w, &sum);
    char row[256];
    if (csv_format_summary(row, sizeof(row), s_meta.id, s_meta.start_utc, s_meta.workout, s_meta.ftp_w, &sum,
                           app->version)) {
        ok = append(RIDES_DIR "/summary.csv", CSV_SUMMARY_HEADER, row) && ok;
    }
    int16_t bests[METRICS_N_BESTS];
    metrics_bests(s_buf, s_n, bests);
    char brows[METRICS_N_BESTS * 40];
    if (csv_format_bests(brows, sizeof(brows), s_meta.id, bests)) {
        ok = append(RIDES_DIR "/bests.csv", CSV_BESTS_HEADER, brows) && ok;
    }
    if (ok) {
        remove(JOURNAL);  // only when everything is on the card
    }
    ESP_LOGI(TAG, "ride %s saved (%u s)%s", s_meta.id, (unsigned)s_n, ok ? "" : " WITH ERRORS");
    return ok;
}

bool recorder_finish(void)
{
    flush();
    if (s_csv) {
        fclose(s_csv);
        s_csv = NULL;
    }
    if (!s_buf) {
        return false;
    }
    if (s_write_error && s_n >= RECORDER_MIN_S) {
        // Rewrite the whole CSV from memory in case the card came back
        char p[64];
        path(p, sizeof(p), "csv");
        FILE *f = fopen(p, "w");
        if (f) {
            fputs(CSV_RIDE_HEADER, f);
            char row[CSV_ROW_MAX];
            for (size_t i = 0; i < s_n; i++) {
                if (csv_format_row(row, sizeof(row), &s_buf[i])) {
                    fputs(row, f);
                }
            }
            fclose(f);
        }
    }
    return write_outputs();
}

void recorder_recover(void)
{
    FILE *j = fopen(JOURNAL, "r");
    if (!j || !s_buf) {
        if (j) {
            fclose(j);
        }
        return;
    }
    char line[64];
    memset(&s_meta, 0, sizeof(s_meta));
    bool ok = fgets(s_meta.id, sizeof(s_meta.id), j) && fgets(line, sizeof(line), j);
    if (ok) {
        s_meta.start_utc = (uint32_t)strtoul(line, NULL, 10);
        ok = fgets(line, sizeof(line), j) != NULL;
        s_meta.ftp_w = ok ? (uint16_t)atoi(line) : 0;
        if (!fgets(s_meta.workout, sizeof(s_meta.workout), j)) {
            s_meta.workout[0] = 0;
        }
    }
    fclose(j);
    s_meta.id[strcspn(s_meta.id, "\r\n")] = 0;
    s_meta.workout[strcspn(s_meta.workout, "\r\n")] = 0;
    if (!ok || strlen(s_meta.id) != 15 || strchr(s_meta.id, '/')) {
        ESP_LOGW(TAG, "journal unreadable, removed");
        remove(JOURNAL);
        return;
    }

    char p[64];
    path(p, sizeof(p), "csv");
    FILE *f = fopen(p, "r");
    s_n = 0;
    char row[CSV_ROW_MAX + 2];
    while (f && s_n < RECORDER_MAX_S && fgets(row, sizeof(row), f)) {
        if (csv_parse_row(row, &s_buf[s_n])) {
            s_n++;
        }
    }
    if (f) {
        fclose(f);
    }
    ESP_LOGW(TAG, "recovering interrupted ride %s (%u s)", s_meta.id, (unsigned)s_n);
    write_outputs();
}
