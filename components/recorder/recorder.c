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
    rec_info_t info;
    int16_t bests[METRICS_N_BESTS];
} s_meta;

void recorder_init(void)
{
    s_buf = heap_caps_malloc(RECORDER_MAX_S * sizeof(ride_sample_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "no PSRAM for the ride buffer");
    }
}

// <user dir>/<name>, e.g. /sdcard/rides/u1/summary.csv
static void user_path(char *out, size_t size, const char *name)
{
    snprintf(out, size, RIDES_DIR "/%s/%s", s_meta.info.user_id, name);
}

static void path(char *out, size_t size, const char *ext)
{
    snprintf(out, size, RIDES_DIR "/%s/%s.%s", s_meta.info.user_id, s_meta.id, ext);
}

// Lowercase letters and digits only: the id is a folder name and goes into the journal
static bool valid_user_id(const char *id)
{
    size_t n = strlen(id);
    return n > 0 && n < sizeof(s_meta.info.user_id) && strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789") == n;
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

bool recorder_start(uint32_t start_utc, const rec_info_t *info)
{
    if (!s_buf || !valid_user_id(info->user_id)) {
        return false;
    }
    memset(&s_meta, 0, sizeof(s_meta));
    s_meta.start_utc = start_utc;
    s_meta.info = *info;
    // One line, no newline inside: the journal is line-based
    s_meta.info.workout[strcspn(s_meta.info.workout, "\r\n")] = 0;
    mkdir(RIDES_DIR, 0775);
    char p[64];
    snprintf(p, sizeof(p), RIDES_DIR "/%s", s_meta.info.user_id);
    mkdir(p, 0775);
    // Ride ids are local time: after a DST change (or a clock fix) the same
    // minute can come round again. Never reuse the name of an existing ride.
    int64_t local = (int64_t)start_utc + info->utc_offset_s;
    for (int i = 0; i < 60; i++, local += 60) {
        struct stat st;
        csv_ride_id(s_meta.id, (uint32_t)local);
        path(p, sizeof(p), "csv");
        bool taken = stat(p, &st) == 0;
        path(p, sizeof(p), "fit");
        if (!taken && stat(p, &st) != 0) {
            break;
        }
    }
    s_n = 0;
    s_pending_len = 0;
    s_write_error = false;

    path(p, sizeof(p), "csv");
    s_csv = fopen(p, "w");
    if (s_csv) {
        fputs(CSV_RIDE_HEADER, s_csv);
    }
    FILE *j = fopen(JOURNAL, "w");
    if (j) {
        const rec_info_t *i = &s_meta.info;
        fprintf(j, "%s %lu %s %u %u %u %ld\n%s\n", s_meta.id, (unsigned long)start_utc, i->user_id, i->ftp_w,
                i->weight_kg, i->max_hr, (long)i->utc_offset_s, i->workout);
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
    const rec_info_t *info = &s_meta.info;
    fit_meta_t meta = {.ftp_w = info->ftp_w, .sw_version = 100, .utc_offset_s = info->utc_offset_s};
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
    metrics_summary(&m, info->ftp_w, &sum);
    // Rows already there come from an earlier attempt (e.g. the FIT write
    // failed and this is the recovery at boot): never add a ride twice.
    char row[256];
    user_path(p, sizeof(p), "summary.csv");
    if (!csv_has_ride(p, s_meta.id) &&
        csv_format_summary(row, sizeof(row), s_meta.id, s_meta.start_utc, info->workout, info->ftp_w, &sum,
                           app->version, info->weight_kg, info->max_hr)) {
        ok = append(p, CSV_SUMMARY_HEADER, row) && ok;
    }
    metrics_bests(s_buf, s_n, s_meta.bests);
    char brows[METRICS_N_BESTS * 40];
    user_path(p, sizeof(p), "bests.csv");
    if (!csv_has_ride(p, s_meta.id) && csv_format_bests(brows, sizeof(brows), s_meta.id, s_meta.bests)) {
        ok = append(p, CSV_BESTS_HEADER, brows) && ok;
    }
    if (ok) {
        remove(JOURNAL);  // only when everything is on the card
    }
    ESP_LOGI(TAG, "ride %s/%s saved (%u s)%s", info->user_id, s_meta.id, (unsigned)s_n, ok ? "" : " WITH ERRORS");
    return ok;
}

bool recorder_finish(int16_t bests[METRICS_N_BESTS])
{
    for (int b = 0; b < METRICS_N_BESTS; b++) {
        bests[b] = -1;
    }
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
    bool saved = write_outputs();
    if (s_n >= RECORDER_MIN_S) {
        // Even if the SD card failed: an FTP test's result is still worth offering
        memcpy(bests, s_meta.bests, sizeof(s_meta.bests));
    }
    return saved;
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
    char line[96];
    unsigned long start = 0;
    unsigned ftp = 0, weight = 0, max_hr = 0;
    long offset = 0;  // missing in journals from older firmware
    memset(&s_meta, 0, sizeof(s_meta));
    rec_info_t *info = &s_meta.info;
    bool ok = fgets(line, sizeof(line), j) && sscanf(line, "%15s %lu %7s %u %u %u %ld", s_meta.id, &start,
                                                     info->user_id, &ftp, &weight, &max_hr, &offset) >= 6;
    if (ok && fgets(info->workout, sizeof(info->workout), j)) {
        info->workout[strcspn(info->workout, "\r\n")] = 0;
    }
    fclose(j);
    s_meta.start_utc = (uint32_t)start;
    info->ftp_w = (uint16_t)(ftp <= 600 ? ftp : 0);
    info->weight_kg = (uint8_t)(weight <= 255 ? weight : 0);
    info->max_hr = (uint8_t)(max_hr <= 255 ? max_hr : 0);
    info->utc_offset_s = (int32_t)(offset >= -86400 && offset <= 86400 ? offset : 0);
    if (!ok || strlen(s_meta.id) != 15 || strchr(s_meta.id, '/') || !valid_user_id(info->user_id)) {
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
    ESP_LOGW(TAG, "recovering interrupted ride %s/%s (%u s)", info->user_id, s_meta.id, (unsigned)s_n);
    write_outputs();
}

// ---- Racing an earlier ride ---------------------------------------------------------

static bool load_ghost(const char *user_id, const char *ride_id, ghost_t *g)
{
    char p[96], row[CSV_ROW_MAX + 8];
    snprintf(p, sizeof(p), RIDES_DIR "/%s/%s.csv", user_id, ride_id);
    FILE *f = fopen(p, "r");
    if (!f) {
        return false;
    }
    ghost_begin(g);
    ride_sample_t s;
    while (fgets(row, sizeof(row), f)) {
        if (csv_parse_row(row, &s) && !ghost_add(g, &s)) {
            break;  // longer than GHOST_MAX_S: the start is enough to race
        }
    }
    fclose(f);
    return g->n > 0;
}

bool recorder_find_ghost(const char *user_id, const char *route_name, uint32_t len_m, ghost_t *g)
{
    char p[96], line[256];
    snprintf(p, sizeof(p), RIDES_DIR "/%s/summary.csv", user_id);
    FILE *f = fopen(p, "r");
    if (!f || !route_name[0] || len_m == 0) {
        if (f) {
            fclose(f);
        }
        return false;
    }
    // Candidates: whole route ridden, best average speed first (only a hint: the
    // ride may have gone on after the end, so each one is measured below)
    struct {
        char id[24];
        uint32_t mps1000;
    } top[RECORDER_GHOST_TRIES] = {0};
    ghost_summary_t row;
    while (fgets(line, sizeof(line), f)) {
        if (!ghost_summary_row(line, &row) || strcmp(row.workout, route_name) != 0 || row.distance_m < len_m ||
            row.duration_s == 0) {
            continue;
        }
        uint32_t v = (uint32_t)((uint64_t)row.distance_m * 1000 / row.duration_s);
        for (int i = 0; i < RECORDER_GHOST_TRIES; i++) {
            if (v > top[i].mps1000) {
                memmove(&top[i + 1], &top[i], sizeof(top[0]) * (RECORDER_GHOST_TRIES - 1 - i));
                snprintf(top[i].id, sizeof(top[i].id), "%s", row.ride_id);
                top[i].mps1000 = v;
                break;
            }
        }
    }
    fclose(f);
    int best = -1;
    int32_t best_s = 0;
    for (int i = 0; i < RECORDER_GHOST_TRIES && top[i].id[0]; i++) {
        int32_t t = load_ghost(user_id, top[i].id, g) ? ghost_time_at(g, len_m) : -1;
        if (t > 0 && (best < 0 || t < best_s)) {
            best = i;
            best_s = t;
        }
    }
    if (best < 0 || !load_ghost(user_id, top[best].id, g)) {
        return false;
    }
    ESP_LOGI(TAG, "racing %s (%ld s for %lu m)", top[best].id, (long)best_s, (unsigned long)len_m);
    return true;
}
