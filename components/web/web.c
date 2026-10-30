#include "web.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include "bsp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ics.h"
#include "mdns.h"
#include "workout.h"

static const char *TAG = "web";

#define HOSTNAME "trainer"
#define AP_SSID "JustRide"
#define STA_TIMEOUT_US (15 * 1000000)
#define CHUNK 4096
#define OTA_WINDOW_US (5 * 60 * 1000000LL)
#define PLAN_MAX_BYTES (32 * 1024)
#define WWW_MAX_BYTES (16 * 1024 * 1024)  // DuckDB's wasm is ~8 MB gzipped
#define APP_INDEX BSP_SD_MOUNT "/www/stats/index.html"
#define CLOCK_VALID_AFTER 1735689600  // 2025-01-01, as in app.c
#define USERS_MAX_BYTES 512
// An upload from a phone that left Wi-Fi times out forever; give up so the
// server (and web_stop() at ride start, which waits for it) never hangs.
#define MAX_RECV_TIMEOUTS 5

static esp_netif_t *s_sta, *s_ap;
static httpd_handle_t s_http;
static esp_timer_handle_t s_fallback;
static bool s_running, s_is_ap, s_got_ip, s_sntp, s_ntp_synced;
static const web_users_t *s_users;
static char s_ip[16];
static char s_ap_pass[64];  // WPA2: up to 63 characters
static int64_t s_ota_until;
static SemaphoreHandle_t s_lock;  // web_start/web_stop come from the UI and the control task
static char s_buf[CHUNK];         // shared by all handlers: the server runs one handler at a time

// ---- Request checks -------------------------------------------------------------

// DNS rebinding: a page on evil.example that re-resolves to our IP would be
// same-origin with us. Only serve requests addressed to our name or an IPv4 literal.
static bool host_ok(httpd_req_t *req)
{
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    host[strcspn(host, ":")] = 0;  // drop the port
    if (strcasecmp(host, HOSTNAME ".local") == 0 || strcasecmp(host, HOSTNAME) == 0) {
        return true;
    }
    int dots = 0;
    for (const char *c = host; *c; c++) {
        if (*c == '.') {
            dots++;
        } else if (!isdigit((unsigned char)*c)) {
            return false;
        }
    }
    return dots == 3;
}

// Answers and returns false if the request must not be served. POSTs and DELETEs
// need a custom header: browsers send one cross-origin only after a CORS preflight,
// which this server never grants, so other web pages can't upload, delete or flash.
static bool allowed(httpd_req_t *req)
{
    char v[8];
    if (!host_ok(req)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown host");
        return false;
    }
    if (req->method != HTTP_GET && httpd_req_get_hdr_value_str(req, "X-JustRide", v, sizeof(v)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "missing X-JustRide header");
        return false;
    }
    return true;
}

// In-place %XX decoding of a URI path segment. False on malformed escapes or %00.
static bool url_decode(char *s)
{
    char *out = s;
    for (const char *in = s; *in; in++) {
        if (*in != '%') {
            *out++ = *in;
            continue;
        }
        unsigned v;
        if (!isxdigit((unsigned char)in[1]) || !isxdigit((unsigned char)in[2]) || sscanf(in + 1, "%2x", &v) != 1 ||
            v == 0) {
            return false;
        }
        *out++ = (char)v;
        in += 2;
    }
    *out = 0;
    return true;
}

// ---- Files ----------------------------------------------------------------------

static bool safe_name(const char *name)
{
    if (!*name || strlen(name) > 64 || strstr(name, "..")) {
        return false;
    }
    for (const char *c = name; *c; c++) {
        bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                  strchr("._- /", *c);
        if (!ok) {
            return false;
        }
    }
    return true;
}

static const char *mime(const char *path)
{
    static const char *map[][2] = {
        {".html", "text/html"},  {".js", "application/javascript"}, {".mjs", "application/javascript"},
        {".css", "text/css"},    {".wasm", "application/wasm"},     {".json", "application/json"},
        {".csv", "text/csv"},    {".svg", "image/svg+xml"},         {".zwo", "application/xml"},
    };
    const char *dot = strrchr(path, '.');
    for (size_t i = 0; dot && i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(dot, map[i][0]) == 0) {
            return map[i][1];
        }
    }
    return "application/octet-stream";
}

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *cache)
{
    char real[160];
    struct stat st;
    bool gz = false;
    snprintf(real, sizeof(real), "%s.gz", path);
    if (stat(real, &st) == 0) {
        gz = true;
    } else {
        snprintf(real, sizeof(real), "%s", path);
        if (stat(real, &st) != 0 || S_ISDIR(st.st_mode)) {
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        }
    }

    char etag[40];
    snprintf(etag, sizeof(etag), "\"%lx-%lx\"", (unsigned long)st.st_size, (unsigned long)st.st_mtime);
    char inm[40];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && strcmp(inm, etag) == 0) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    FILE *f = fopen(real, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    httpd_resp_set_type(req, mime(path));
    httpd_resp_set_hdr(req, "Cache-Control", cache);
    httpd_resp_set_hdr(req, "ETag", etag);
    if (gz) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    size_t n;
    esp_err_t err = ESP_OK;
    while (err == ESP_OK && (n = fread(s_buf, 1, sizeof(s_buf), f)) > 0) {
        err = httpd_resp_send_chunk(req, s_buf, (ssize_t)n);
    }
    fclose(f);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

// ---- Handlers -------------------------------------------------------------------

// Shown only when the web app is missing from the SD card
static const char INDEX_HTML[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>JustRide</title><style>body{font:16px/1.5 system-ui;max-width:36em;margin:3em auto;padding:0 1em;"
    "background:#0b0d12;color:#e7e9ee}code{background:#1b1f29;padding:2px 6px;border-radius:6px}</style>"
    "<h1>JustRide</h1><p>The web app is not on the SD card. Put the card in your computer and run "
    "<code>tools/sd_deploy.sh /Volumes/JUSTRIDE</code>, then put it back and reload this page.</p>"
    "<p>Ride files: <a href='/api/rides'>/api/rides</a></p>";

static esp_err_t h_index(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    struct stat st;
    if (stat(APP_INDEX, &st) == 0 || stat(APP_INDEX ".gz", &st) == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/stats/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_stats(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    const char *rel = req->uri + strlen("/stats/");
    char name[80];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(rel, "?#"), rel);
    if (!*name) {
        strcpy(name, "index.html");
    }
    if (!safe_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[128];
    snprintf(path, sizeof(path), BSP_SD_MOUNT "/www/stats/%s", name);
    // vendor/ paths carry the library version, so they never change
    const char *cache = strncmp(name, "vendor/", 7) == 0 ? "max-age=31536000, immutable" : "no-cache";
    return send_file(req, path, cache);
}

static esp_err_t h_rides_file(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    // <user id>/<file>
    const char *name = req->uri + strlen("/rides/");
    const char *slash = strchr(name, '/');
    if (!safe_name(name) || !slash || slash == name || strchr(slash + 1, '/') || !slash[1]) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[128];
    snprintf(path, sizeof(path), BSP_SD_MOUNT "/rides/%s", name);
    return send_file(req, path, "no-cache");
}

// Every file in every rider's folder: [{"name":"u1/2026-10-26_1200.fit",...}]
static esp_err_t h_api_rides(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    DIR *top = opendir(BSP_SD_MOUNT "/rides");
    struct dirent *u;
    while (top && (u = readdir(top))) {
        char dir[160];
        if (u->d_name[0] == '.' || !safe_name(u->d_name) || strlen(u->d_name) > 8) {
            continue;
        }
        snprintf(dir, sizeof(dir), BSP_SD_MOUNT "/rides/%s", u->d_name);
        DIR *d = opendir(dir);  // NULL for plain files
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.' || !safe_name(e->d_name)) {
                continue;
            }
            char path[300], item[200];
            struct stat st;
            snprintf(path, sizeof(path), "%s/%.64s", dir, e->d_name);  // safe_name: <= 64
            if (stat(path, &st) != 0) {
                continue;
            }
            snprintf(item, sizeof(item), "%s{\"name\":\"%.8s/%.64s\",\"size\":%ld,\"mtime\":%ld}", first ? "" : ",",
                     u->d_name, e->d_name, (long)st.st_size, (long)st.st_mtime);
            httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
        if (d) {
            closedir(d);
        }
    }
    if (top) {
        closedir(top);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// Riders from settings.json: [{"id":"u1","name":"Sam","active":true}] (nothing else from that file)
static esp_err_t send_users(httpd_req_t *req, const settings_t *s)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s->n_users; i++) {
        const user_t *u = &s->users[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", u->id);
        cJSON_AddStringToObject(o, "name", u->name);
        cJSON_AddBoolToObject(o, "active", i == s->user);
        cJSON_AddNumberToObject(o, "ftp_w", u->ftp_w);
        cJSON_AddBoolToObject(o, "ftp_known", u->ftp_known);
        cJSON_AddNumberToObject(o, "max_erg_w", u->max_erg_w);
        cJSON_AddNumberToObject(o, "weight_kg", u->weight_kg);
        cJSON_AddNumberToObject(o, "max_hr", u->max_hr);
        cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    esp_err_t err = text ? httpd_resp_sendstr(req, text) : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "");
    free(text);
    return err;
}

static esp_err_t h_api_users(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    settings_t *s = malloc(sizeof(*s));
    if (!s) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (s_users) {
        s_users->get(s);
    } else {
        settings_read_users(s);
    }
    esp_err_t err = send_users(req, s);
    free(s);
    return err;
}

static int json_int(const cJSON *o, const char *key, int fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : fallback;
}

// {"action": "add", "name"} | {"action": "edit", "id", "name", "ftp_w", "max_erg_w", "weight_kg", "max_hr"}
// | {"action": "select", "id"} | {"action": "delete", "id"}. Answers with the new rider list.
static esp_err_t h_api_users_post(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    if (!s_users) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not available");
    }
    if (req->content_len == 0 || req->content_len > USERS_MAX_BYTES) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad request");
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, s_buf + got, req->content_len - got);
        if (n <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload interrupted");
        }
        got += n;
    }
    cJSON *o = cJSON_ParseWithLength(s_buf, got);
    const char *action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "action"));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "id"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "name"));
    settings_t *s = malloc(sizeof(*s));
    bool ok = false;
    const char *why = "not possible during a ride";
    if (!o || !action || !s) {
        why = "bad request";
    } else if (strcmp(action, "add") == 0) {
        ok = name && s_users->add(name);
        why = "name invalid, or already 8 riders";
    } else if (strcmp(action, "select") == 0) {
        ok = id && s_users->select(id);
    } else if (strcmp(action, "delete") == 0) {
        ok = id && s_users->remove(id);
        why = "the last rider can't be deleted";
    } else if (strcmp(action, "edit") == 0 && id) {
        s_users->get(s);
        int idx = settings_find_user(s, id);
        if (idx >= 0) {
            user_t u = s->users[idx];
            if (name) {
                snprintf(u.name, sizeof(u.name), "%s", name);
            }
            // Out-of-range values are refused by edit() (it checks them all)
            int ftp = json_int(o, "ftp_w", u.ftp_w), erg = json_int(o, "max_erg_w", u.max_erg_w);
            int kg = json_int(o, "weight_kg", u.weight_kg), hr = json_int(o, "max_hr", u.max_hr);
            ok = ftp > 0 && ftp < 65536 && erg > 0 && erg < 65536 && kg > 0 && kg < 256 && hr > 0 && hr < 256;
            u.ftp_w = (uint16_t)ftp;
            u.max_erg_w = (uint16_t)erg;
            u.weight_kg = (uint8_t)kg;
            u.max_hr = (uint8_t)hr;
            ok = ok && s_users->edit(&u);
            why = "value out of range";
        }
    }
    cJSON_Delete(o);
    esp_err_t err;
    if (ok) {
        s_users->get(s);
        err = send_users(req, s);
    } else {
        err = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
    }
    free(s);
    return err;
}

// The browser's clock, for a device that can't reach a time server (body: UTC
// seconds). Ignored once NTP has set the clock; small differences are ignored too.
static esp_err_t h_api_time(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char body[24] = {0};
    int n = req->content_len < sizeof(body) ? httpd_req_recv(req, body, sizeof(body) - 1) : 0;
    char *end;
    long long t = n > 0 ? strtoll(body, &end, 10) : 0;
    if (t < CLOCK_VALID_AFTER || t > 4102444800LL) {  // 2100
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time");
    }
    time_t now = time(NULL);
    if (!s_ntp_synced && (now < CLOCK_VALID_AFTER || llabs(t - (long long)now) > 120)) {
        struct timeval tv = {.tv_sec = (time_t)t};
        settimeofday(&tv, NULL);
        bsp_rtc_set((time_t)t);
        ESP_LOGI(TAG, "clock set from the web app");
        return httpd_resp_sendstr(req, "set");
    }
    return httpd_resp_sendstr(req, "kept");
}

// Request body -> path, via a temp file so a broken upload never replaces a good file
static esp_err_t receive_file(httpd_req_t *req, const char *path, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file empty or too large");
    }
    char tmp[136];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SD not writable");
    }
    size_t left = req->content_len;
    bool ok = true;
    int timeouts = 0;
    while (ok && left > 0) {
        int n = httpd_req_recv(req, s_buf, left < sizeof(s_buf) ? left : sizeof(s_buf));
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;  // timeouts in a row; any data resets the count
        }
        timeouts = 0;
        ok = n > 0 && fwrite(s_buf, 1, (size_t)n, f) == (size_t)n;
        left -= ok ? (size_t)n : 0;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    } else {
        remove(tmp);
    }
    return ok ? httpd_resp_sendstr(req, "saved") : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed");
}

// /workouts/<name>.zwo -> SD path. The browser sends encodeURIComponent(name).
static bool workout_path(httpd_req_t *req, char *path, size_t size)
{
    char name[80];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(req->uri + strlen("/workouts/"), "?#"),
             req->uri + strlen("/workouts/"));
    size_t len = url_decode(name) ? strlen(name) : 0;
    if (!safe_name(name) || strchr(name, '/') || len < 5 || strcasecmp(name + len - 4, ".zwo") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name must be <name>.zwo");
        return false;
    }
    snprintf(path, size, BSP_SD_MOUNT "/workouts/%s", name);
    return true;
}

static esp_err_t h_workout_upload(httpd_req_t *req)
{
    char path[128];
    if (!allowed(req) || !workout_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    mkdir(BSP_SD_MOUNT "/workouts", 0775);
    return receive_file(req, path, ZWO_MAX_BYTES);
}

static esp_err_t h_workout_get(httpd_req_t *req)
{
    char path[128];
    if (!allowed(req) || !workout_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    return send_file(req, path, "no-cache");
}

static esp_err_t h_workout_delete(httpd_req_t *req)
{
    char path[128];
    if (!allowed(req) || !workout_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    return remove(path) == 0 ? httpd_resp_sendstr(req, "deleted")
                             : httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

// ---- "Add to calendar" ----------------------------------------------------------

#define WORKOUT_DIR BSP_SD_MOUNT "/workouts"
#define ALARM_MIN 15

static void workout_title(char *out, size_t size, const char *file)
{
    snprintf(out, size, "%.*s", (int)(strlen(file) - 4), file);
    for (char *c = out; *c; c++) {
        *c = *c == '_' ? ' ' : *c;
    }
}

static bool workout_file_ok(const char *name)
{
    size_t len = strlen(name);
    return safe_name(name) && !strchr(name, '/') && name[0] != '.' && len >= 5 && len <= 60 &&
           strcasecmp(name + len - 4, ".zwo") == 0;
}

// xml: ZWO_MAX_BYTES of scratch space
static bool read_workout(const char *file, char *xml, workout_t *w)
{
    char path[128];
    snprintf(path, sizeof(path), WORKOUT_DIR "/%.60s", file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(xml, 1, ZWO_MAX_BYTES, f);
    fclose(f);
    return zwo_parse(xml, n, w);
}

// GET /api/workout.ics?file=<name>.zwo&start=<unix time>[&user=<id>]: one calendar
// event (the web app's "Add to calendar"). Served as text/calendar so the phone or
// computer offers to add it.
static esp_err_t h_workout_ics(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    char query[300] = "", file[200] = "", start_s[16] = "", user_id[8] = "";  // file: %-encoded, up to 60 chars
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "file", file, sizeof(file));
    httpd_query_key_value(query, "start", start_s, sizeof(start_s));
    httpd_query_key_value(query, "user", user_id, sizeof(user_id));
    char *end;
    unsigned long start = strtoul(start_s, &end, 10);
    if (!url_decode(file) || !workout_file_ok(file) || !start_s[0] || *end || start < 1735689600 ||
        start > 4000000000UL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need file=<name>.zwo and start=<unix time>");
    }
    char *xml = malloc(ZWO_MAX_BYTES);
    workout_t *w = malloc(sizeof(*w));
    settings_t *s = malloc(sizeof(*s));
    char *ics = malloc(4096);
    esp_err_t err;
    if (!xml || !w || !s || !ics) {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    } else if (!read_workout(file, xml, w)) {
        err = httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "workout not found or not readable");
    } else {
        workout_est_t est;
        workout_estimate(w, &est);
        settings_read_users(s);
        const user_t *u = NULL;
        for (int i = 0; i < s->n_users; i++) {
            if (strcmp(s->users[i].id, user_id) == 0 || (!user_id[0] && i == s->user)) {
                u = &s->users[i];
            }
        }
        char title[64], summary[96], who[96] = "", desc[400], uid[96];
        workout_title(title, sizeof(title), file);
        snprintf(summary, sizeof(summary), "Ride: %s", w->name[0] ? w->name : title);
        if (u) {
            snprintf(who, sizeof(who), "%.15s: about %lu W average at FTP %u W\n", u->name,
                     (unsigned long)(u->ftp_w * est.avg_pm + 500) / 1000, u->ftp_w);
        }
        snprintf(desc, sizeof(desc), "%lu min, TSS %u, IF %u.%02u\n%sOn the JustRide head unit: Workouts > %s",
                 (unsigned long)(est.total_s + 30) / 60, (est.tss10 + 5) / 10, est.if_pm / 1000,
                 est.if_pm % 1000 / 10, who, title);
        snprintf(uid, sizeof(uid), "%lu-%.60s@justride", start, file);
        time_t now = time(NULL);
        ics_event_t ev = {.uid = uid, .stamp_utc = now > 1735689600 ? (uint32_t)now : (uint32_t)start,
                          .start_utc = (uint32_t)start, .duration_s = est.total_s, .summary = summary,
                          .description = desc, .alarm_min = ALARM_MIN};
        size_t len = ics_write(ics, 4096, &ev);
        if (len == 0) {
            err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "event too large");
        } else {
            char disp[96];
            snprintf(disp, sizeof(disp), "inline; filename=\"%.60s.ics\"", title);
            httpd_resp_set_type(req, "text/calendar; charset=utf-8");
            httpd_resp_set_hdr(req, "Content-Disposition", disp);
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            err = httpd_resp_send(req, ics, (ssize_t)len);
        }
    }
    free(xml);
    free(w);
    free(s);
    free(ics);
    return err;
}

// [{"name":"Sweet Spot 3x10.zwo","size":512,"mtime":...}]
static esp_err_t h_api_workouts(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    DIR *d = opendir(BSP_SD_MOUNT "/workouts");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.' || !safe_name(e->d_name) || len < 5 || strcasecmp(e->d_name + len - 4, ".zwo") != 0) {
            continue;
        }
        char path[160], item[160];
        struct stat st;
        snprintf(path, sizeof(path), BSP_SD_MOUNT "/workouts/%.64s", e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        snprintf(item, sizeof(item), "%s{\"name\":\"%.64s\",\"size\":%ld,\"mtime\":%ld}", first ? "" : ",", e->d_name,
                 (long)st.st_size, (long)st.st_mtime);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    if (d) {
        closedir(d);
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

// /plans/<user id>.json: the training plan the web app made for a rider (the app owns its format)
static bool plan_path(httpd_req_t *req, char *path, size_t size)
{
    char name[24];
    snprintf(name, sizeof(name), "%.*s", (int)strcspn(req->uri + strlen("/plans/"), "?#"), req->uri + strlen("/plans/"));
    size_t len = strlen(name);
    bool ok = len > 5 && len <= 8 + 5 && strcmp(name + len - 5, ".json") == 0;
    for (size_t i = 0; ok && i < len - 5; i++) {
        ok = isalnum((unsigned char)name[i]);
    }
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name must be <user id>.json");
        return false;
    }
    snprintf(path, size, BSP_SD_MOUNT "/plans/%s", name);
    return true;
}

static esp_err_t h_plan_get(httpd_req_t *req)
{
    char path[64];
    if (!allowed(req) || !plan_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    return send_file(req, path, "no-cache");
}

static esp_err_t h_plan_upload(httpd_req_t *req)
{
    char path[64];
    if (!allowed(req) || !plan_path(req, path, sizeof(path))) {
        return ESP_OK;
    }
    mkdir(BSP_SD_MOUNT "/plans", 0775);
    return receive_file(req, path, PLAN_MAX_BYTES);
}

// Web app update over Wi-Fi (tools/web_deploy.sh): only inside the window opened by
// "Allow firmware update" on the device, so nobody else on the network can replace the app.
static esp_err_t h_www_upload(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    if (!web_ota_allowed()) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "Tap 'Allow firmware update' on the device first (valid 5 minutes)");
    }
    const char *rel = req->uri + strlen("/www/stats/");
    if (strncmp(req->uri, "/www/stats/", 11) != 0 || !safe_name(rel) || rel[strlen(rel) - 1] == '/') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[128];
    snprintf(path, sizeof(path), BSP_SD_MOUNT "/www/stats/%.64s", rel);  // safe_name: <= 64
    for (char *p = path + strlen(BSP_SD_MOUNT) + 1; (p = strchr(p, '/')); p++) {  // mkdir -p
        *p = 0;
        mkdir(path, 0775);
        *p = '/';
    }
    return receive_file(req, path, WWW_MAX_BYTES);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

// The image is checked twice: our project name here, and esp_ota_end() verifies
// the image format, chip and checksum. The bootloader rolls back if the new
// firmware doesn't mark itself valid after a successful start.
static esp_err_t h_ota(httpd_req_t *req)
{
    if (!allowed(req)) {
        return ESP_OK;
    }
    if (!web_ota_allowed()) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                                   "Tap 'Allow firmware update' on the device first (valid 5 minutes)");
    }
    s_ota_until = 0;  // one upload per tap
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad image size");
    }
    esp_ota_handle_t ota;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
    }
    char *buf = s_buf;
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t head = desc_off + sizeof(esp_app_desc_t);
    size_t left = req->content_len, done = 0, have = 0;
    const char *err = NULL;
    int timeouts = 0;
    while (!err && left > 0) {
        int n = httpd_req_recv(req, buf + have, left < CHUNK - have ? left : CHUNK - have);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < MAX_RECV_TIMEOUTS) {
            continue;  // timeouts in a row; any data resets the count
        }
        timeouts = 0;
        if (n <= 0) {
            err = "upload interrupted";
            break;
        }
        left -= (size_t)n;
        have += (size_t)n;
        if (done == 0) {
            if (have < head && left > 0) {
                continue;  // collect the image header before checking it
            }
            const esp_app_desc_t *new_app = (const esp_app_desc_t *)(buf + desc_off);
            if (have < head || new_app->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(new_app->project_name, esp_app_get_description()->project_name,
                        sizeof(new_app->project_name)) != 0) {
                err = "not a JustRide firmware";
                break;
            }
        }
        if (esp_ota_write(ota, buf, have) != ESP_OK) {
            err = "write failed";
        }
        done += have;
        have = 0;
    }
    if (err) {
        esp_ota_abort(ota);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    }
    if (esp_ota_end(ota) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image invalid");
    }
    ESP_LOGI(TAG, "OTA ok (%u bytes), restarting", (unsigned)done);
    httpd_resp_sendstr(req, "OK, restarting");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 20;
    if (httpd_start(&s_http, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/stats/*", .method = HTTP_GET, .handler = h_stats},
        {.uri = "/api/rides", .method = HTTP_GET, .handler = h_api_rides},
        {.uri = "/api/users", .method = HTTP_GET, .handler = h_api_users},
        {.uri = "/api/users", .method = HTTP_POST, .handler = h_api_users_post},
        {.uri = "/api/time", .method = HTTP_POST, .handler = h_api_time},
        {.uri = "/api/workout.ics", .method = HTTP_GET, .handler = h_workout_ics},
        {.uri = "/rides/*", .method = HTTP_GET, .handler = h_rides_file},
        {.uri = "/api/workouts", .method = HTTP_GET, .handler = h_api_workouts},
        {.uri = "/workouts/*", .method = HTTP_GET, .handler = h_workout_get},
        {.uri = "/workouts/*", .method = HTTP_POST, .handler = h_workout_upload},
        {.uri = "/workouts/*", .method = HTTP_DELETE, .handler = h_workout_delete},
        {.uri = "/plans/*", .method = HTTP_GET, .handler = h_plan_get},
        {.uri = "/plans/*", .method = HTTP_POST, .handler = h_plan_upload},
        {.uri = "/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/www/*", .method = HTTP_POST, .handler = h_www_upload},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_http, &uris[i]);
    }
}

// ---- Wi-Fi ----------------------------------------------------------------------

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "clock set from NTP");
    s_ntp_synced = true;
    bsp_rtc_set(tv->tv_sec);
}

static void start_ap(void)
{
    if (strlen(s_ap_pass) < 8) {
        ESP_LOGE(TAG, "AP password too short, not starting the access point");
        return;
    }
    wifi_config_t ap = {.ap = {.ssid = AP_SSID, .ssid_len = strlen(AP_SSID), .channel = 6, .max_connection = 2,
                               .authmode = WIFI_AUTH_WPA2_PSK}};
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_ap_pass);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();
    s_is_ap = true;
    strcpy(s_ip, "192.168.4.1");
    ESP_LOGI(TAG, "access point %s started", AP_SSID);
}

// esp_timer task: take the lock so this can't race web_stop() and turn the
// access point on after a ride started. Don't wait for it (that would delay
// other timers, e.g. the buzzer off); if web_start/stop holds it, skip.
static void fallback_cb(void *arg)
{
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    if (s_running && !s_got_ip) {
        ESP_LOGW(TAG, "home Wi-Fi not reachable, starting own access point");
        start_ap();
    }
    xSemaphoreGive(s_lock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && s_running && !s_is_ap) {
        // 201 no AP found (wrong name, 5 GHz only), 15/204/210 wrong password
        wifi_event_sta_disconnected_t *e = data;
        ESP_LOGW(TAG, "home Wi-Fi: disconnected, reason %d", e->reason);
        s_got_ip = false;
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "access point: device joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = data;
        ESP_LOGW(TAG, "access point: device left, reason %d", e->reason);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        ESP_LOGI(TAG, "got IP %s", s_ip);
        // Time from the router first (home routers usually serve NTP and work
        // without internet), pool.ntp.org second. Start now that DNS and a route exist.
        if (s_sntp) {
            ip_addr_t gw = IPADDR4_INIT(e->ip_info.gw.addr);
            esp_sntp_setserver(0, &gw);
            esp_netif_sntp_start();
        }
    }
}

void web_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, HOSTNAME);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // credentials live in settings.json; no flash writes
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_timer_create_args_t t = {.callback = fallback_cb, .name = "wifi_fb"};
    esp_timer_create(&t, &s_fallback);
    s_lock = xSemaphoreCreateMutex();
}

void web_start(const settings_t *s)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_running = true;
    s_is_ap = s_got_ip = false;
    s_ip[0] = 0;
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%s", s->ap_pass);

    if (s->wifi_ssid[0]) {
        wifi_config_t sta = {0};
        memcpy(sta.sta.ssid, s->wifi_ssid, strlen(s->wifi_ssid));  // a 32-byte SSID has no NUL
        snprintf((char *)sta.sta.password, sizeof(sta.sta.password), "%s", s->wifi_pass);
        // WPA3 routers often require SAE hash-to-element; without it auth fails (reason 202)
        sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        sta.sta.pmf_cfg.capable = true;
        // Length only, never the password: catches stray spaces or a truncated value
        ESP_LOGI(TAG, "connecting to \"%.32s\" (password %u characters)", s->wifi_ssid,
                 (unsigned)strlen(s->wifi_pass));
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_start();
        esp_timer_start_once(s_fallback, STA_TIMEOUT_US);
        esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("0.0.0.0", "pool.ntp.org"));
        sntp.start = false;  // on IP_EVENT_STA_GOT_IP, with the router as server 0
        sntp.sync_cb = on_time_sync;
        s_sntp = esp_netif_sntp_init(&sntp) == ESP_OK;
    } else {
        start_ap();
    }
    http_start();
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
    xSemaphoreGive(s_lock);
}

void web_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_running) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_running = false;
    s_ota_until = 0;
    esp_timer_stop(s_fallback);
    if (s_http) {
        httpd_stop(s_http);
        s_http = NULL;
    }
    mdns_free();
    if (s_sntp) {
        esp_netif_sntp_deinit();
        s_sntp = false;
    }
    esp_wifi_stop();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Wi-Fi off");
}

void web_scan_print(void)
{
    static const char *auth[] = {"open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA2-Enterprise", "WPA3",
                                 "WPA2/WPA3", "WAPI", "OWE", "WPA3-Enterprise-192", "WPA3-ext", "WPA3-ext-mixed"};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_running) {
        xSemaphoreGive(s_lock);
        printf("turn Wi-Fi off first\n");
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    wifi_ap_record_t recs[20];
    uint16_t n = sizeof(recs) / sizeof(recs[0]);
    if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            unsigned a = recs[i].authmode;
            printf("%-32.32s %4d dBm  ch %2u  %s\n", (const char *)recs[i].ssid, recs[i].rssi, recs[i].primary,
                   a < sizeof(auth) / sizeof(auth[0]) ? auth[a] : "?");
        }
    }
    esp_wifi_stop();
    xSemaphoreGive(s_lock);
}

void web_set_users(const web_users_t *u) { s_users = u; }

void web_time_print(void)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    printf("%04d-%02d-%02d %02d:%02d:%02d UTC, %s, NTP %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
           tm.tm_min, tm.tm_sec, now > CLOCK_VALID_AFTER ? "valid" : "not set",
           s_ntp_synced ? "synced" : s_sntp ? "waiting" : "off");
}

void web_allow_ota(void) { s_ota_until = esp_timer_get_time() + OTA_WINDOW_US; }

bool web_ota_allowed(void) { return s_running && esp_timer_get_time() < s_ota_until; }

bool web_running(void) { return s_running; }

void web_status(char *buf, size_t size)
{
    if (!s_running) {
        snprintf(buf, size, "Wi-Fi off");
    } else if (s_is_ap) {
        snprintf(buf, size, "Wi-Fi: %s  Password: %s\nhttp://%s/", AP_SSID, s_ap_pass, s_ip);
    } else if (s_got_ip) {
        snprintf(buf, size, "http://" HOSTNAME ".local/\nhttp://%s/", s_ip);
    } else {
        snprintf(buf, size, "Connecting to Wi-Fi...");
    }
}
