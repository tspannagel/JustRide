#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bsp.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "settings";

#define DIR BSP_SD_MOUNT "/config"
#define SETTINGS_PATH DIR "/settings.json"
#define GEARS_PATH DIR "/gears.json"
#define MAX_JSON 4096

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    char *buf = malloc(MAX_JSON + 1);
    size_t n = buf ? fread(buf, 1, MAX_JSON, f) : 0;
    fclose(f);
    if (buf) {
        buf[n] = 0;
    }
    return buf;
}

static int get_int(const cJSON *o, const char *key, int def, int lo, int hi)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        return def;
    }
    return v->valueint;
}

static double get_num(const cJSON *o, const char *key, double def, double lo, double hi)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) && v->valuedouble >= lo && v->valuedouble <= hi ? v->valuedouble : def;
}

static void get_str(const cJSON *o, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsString(v) && strlen(v->valuestring) < size) {
        strcpy(out, v->valuestring);
    }
}

static void random_pass(char *out, size_t len)
{
    static const char abc[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no look-alikes
    for (size_t i = 0; i < len; i++) {
        out[i] = abc[esp_random() % (sizeof(abc) - 1)];
    }
    out[len] = 0;
}

static void load_gears(gear_table_t *g)
{
    vshift_default(g);
    char *text = read_text(GEARS_PATH);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) {
        return;
    }
    gear_table_t t = {.real_ratio100 = (uint16_t)(get_num(root, "real_ratio", 2.0, 0.5, 6.0) * 100 + 0.5)};
    const cJSON *arr = cJSON_GetObjectItem(root, "ratios");
    bool ok = cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 2 && cJSON_GetArraySize(arr) <= VSHIFT_MAX_GEARS;
    const cJSON *it;
    for (it = ok ? arr->child : NULL; it; it = it->next) {
        double r = cJSON_IsNumber(it) ? it->valuedouble : 0;
        uint16_t r100 = (uint16_t)(r * 100 + 0.5);
        if (r < 0.3 || r > 8.0 || (t.n > 0 && r100 <= t.ratio100[t.n - 1])) {
            ok = false; // must be ascending and sane
            break;
        }
        t.ratio100[t.n++] = r100;
    }
    cJSON_Delete(root);
    if (ok) {
        *g = t;
        ESP_LOGI(TAG, "%u gears from gears.json", t.n);
    } else {
        ESP_LOGW(TAG, "gears.json invalid, using defaults");
    }
}

void settings_load(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ftp_w = 200;
    s->max_erg_w = 600;
    s->crr = 40;
    s->cw = 51;

    char *text = read_text(SETTINGS_PATH);
    if (!text) {
        text = read_text(SETTINGS_PATH ".tmp");  // power cut between remove and rename in settings_save
    }
    bool exists = text != NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    bool valid = root != NULL;
    if (root) {
        s->ftp_w = (uint16_t)get_int(root, "ftp_w", s->ftp_w, 50, 600);
        s->max_erg_w = (uint16_t)get_int(root, "max_erg_w", s->max_erg_w, 100, 1500);
        s->crr = (uint8_t)(get_num(root, "crr", 0.004, 0.001, 0.02) * 10000 + 0.5);
        s->cw = (uint8_t)(get_num(root, "cw", 0.51, 0.1, 2.0) * 100 + 0.5);
        get_str(root, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
        get_str(root, "wifi_password", s->wifi_pass, sizeof(s->wifi_pass));
        get_str(root, "ap_password", s->ap_pass, sizeof(s->ap_pass));
        cJSON_Delete(root);
    } else if (exists) {
        // Never overwrite a file the user is editing: a typo would erase their Wi-Fi settings
        ESP_LOGE(TAG, "settings.json is not valid JSON; using defaults, file left unchanged");
    }
    load_gears(&s->gears);

    bool write = !exists;
    if (strlen(s->ap_pass) < 8) {
        random_pass(s->ap_pass, 10);
        write = write || valid;  // store the new hotspot password
    }
    if (write && bsp_sd_mounted()) {
        settings_save(s);
    }
}

bool settings_save(const settings_t *s)
{
    mkdir(DIR, 0775);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ftp_w", s->ftp_w);
    cJSON_AddNumberToObject(root, "max_erg_w", s->max_erg_w);
    cJSON_AddNumberToObject(root, "crr", s->crr / 10000.0);
    cJSON_AddNumberToObject(root, "cw", s->cw / 100.0);
    cJSON_AddStringToObject(root, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", s->wifi_pass);
    cJSON_AddStringToObject(root, "ap_password", s->ap_pass);
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) {
        return false;
    }

    // Write a temp file, then replace, so a power cut never leaves half a file
    bool ok = false;
    FILE *f = fopen(SETTINGS_PATH ".tmp", "wb");
    if (f) {
        ok = fputs(text, f) >= 0;
        ok = fclose(f) == 0 && ok;
    }
    free(text);
    if (ok) {
        remove(SETTINGS_PATH);
        ok = rename(SETTINGS_PATH ".tmp", SETTINGS_PATH) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "saving settings failed");
    }
    return ok;
}
