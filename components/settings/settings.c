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

bool settings_valid_name(const char *name)
{
    size_t n = strlen(name);
    bool visible = false;
    for (const char *c = name; *c; c++) {
        if (*c < 0x20 || *c > 0x7E) {
            return false;  // the display font is ASCII only
        }
        visible |= *c != ' ';
    }
    return n > 0 && n < SETTINGS_NAME_LEN && visible;
}

static void user_defaults(user_t *u, uint16_t id, const char *name)
{
    memset(u, 0, sizeof(*u));
    snprintf(u->id, sizeof(u->id), "u%u", id);
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->ftp_w = 200;
    u->max_erg_w = 600;
    u->weight_kg = 75;
    u->max_hr = 185;
}

bool settings_add_user(settings_t *s, const char *name)
{
    if (s->n_users >= SETTINGS_MAX_USERS || s->next_id > 9999 || !settings_valid_name(name)) {
        return false;
    }
    user_defaults(&s->users[s->n_users], s->next_id++, name);
    s->user = s->n_users++;
    return true;
}

int settings_find_user(const settings_t *s, const char *id)
{
    for (int i = 0; i < s->n_users; i++) {
        if (strcmp(s->users[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

bool settings_delete_user(settings_t *s, int idx)
{
    if (s->n_users <= 1 || idx < 0 || idx >= s->n_users) {
        return false;
    }
    memmove(&s->users[idx], &s->users[idx + 1], (s->n_users - idx - 1) * sizeof(user_t));
    s->n_users--;
    s->user = idx == s->user ? 0 : idx < s->user ? s->user - 1 : s->user;
    return true;
}

// "u<1..9999>", and not used by an earlier entry
static bool valid_id(const settings_t *s, const char *id, uint16_t *num)
{
    char *end;
    unsigned long n = id[0] == 'u' && id[1] >= '1' && id[1] <= '9' ? strtoul(id + 1, &end, 10) : 0;
    if (n == 0 || n > 9999 || *end) {
        return false;
    }
    for (int i = 0; i < s->n_users; i++) {
        if (strcmp(s->users[i].id, id) == 0) {
            return false;
        }
    }
    *num = (uint16_t)n;
    return true;
}

static void load_users(settings_t *s, const cJSON *root)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "users");
    const cJSON *it;
    for (it = cJSON_IsArray(arr) ? arr->child : NULL; it && s->n_users < SETTINGS_MAX_USERS; it = it->next) {
        user_t u;
        uint16_t num;
        char id[8] = "", name[SETTINGS_NAME_LEN] = "";
        get_str(it, "id", id, sizeof(id));
        get_str(it, "name", name, sizeof(name));
        if (!valid_id(s, id, &num) || !settings_valid_name(name)) {
            ESP_LOGW(TAG, "skipping invalid user entry");
            continue;
        }
        user_defaults(&u, num, name);
        u.ftp_w = (uint16_t)get_int(it, "ftp_w", u.ftp_w, 50, 600);
        u.max_erg_w = (uint16_t)get_int(it, "max_erg_w", u.max_erg_w, 100, 1500);
        u.weight_kg = (uint8_t)get_int(it, "weight_kg", u.weight_kg, 30, 200);
        u.max_hr = (uint8_t)get_int(it, "max_hr", u.max_hr, 100, 230);
        s->users[s->n_users++] = u;
        if (num >= s->next_id) {
            s->next_id = num + 1;
        }
    }
    s->next_id = (uint16_t)get_int(root, "next_user_id", s->next_id, s->next_id, 10000);
    char active[8] = "";
    get_str(root, "user", active, sizeof(active));
    for (int i = 0; i < s->n_users; i++) {
        if (strcmp(s->users[i].id, active) == 0) {
            s->user = (uint8_t)i;
        }
    }
}

void settings_read_users(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    char *text = read_text(SETTINGS_PATH);
    if (!text) {
        text = read_text(SETTINGS_PATH ".tmp");
    }
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (root) {
        load_users(s, root);
        cJSON_Delete(root);
    }
}

void settings_load(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    s->crr = 40;
    s->cw = 51;
    strcpy(s->tz, "UTC0");

    char *text = read_text(SETTINGS_PATH);
    if (!text) {
        text = read_text(SETTINGS_PATH ".tmp");  // power cut between remove and rename in settings_save
    }
    bool exists = text != NULL;
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    bool valid = root != NULL;
    if (root) {
        load_users(s, root);
        if (s->n_users == 0) {
            // First start, or a settings.json from before multi-user: keep its FTP and ERG limit
            user_defaults(&s->users[0], s->next_id++, "Rider");
            s->users[0].ftp_w = (uint16_t)get_int(root, "ftp_w", 200, 50, 600);
            s->users[0].max_erg_w = (uint16_t)get_int(root, "max_erg_w", 600, 100, 1500);
            s->n_users = 1;
        }
        s->crr = (uint8_t)(get_num(root, "crr", 0.004, 0.001, 0.02) * 10000 + 0.5);
        s->cw = (uint8_t)(get_num(root, "cw", 0.51, 0.1, 2.0) * 100 + 0.5);
        get_str(root, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
        get_str(root, "wifi_password", s->wifi_pass, sizeof(s->wifi_pass));
        get_str(root, "ap_password", s->ap_pass, sizeof(s->ap_pass));
        get_str(root, "tz", s->tz, sizeof(s->tz));
        if (!tz_valid(s->tz)) {
            ESP_LOGW(TAG, "tz \"%s\" not supported, using UTC", s->tz);
            strcpy(s->tz, "UTC0");
        }
        cJSON_Delete(root);
    } else if (exists) {
        // Never overwrite a file the user is editing: a typo would erase their Wi-Fi settings
        ESP_LOGE(TAG, "settings.json is not valid JSON; using defaults, file left unchanged");
    }
    if (s->n_users == 0) {  // no or invalid settings.json
        user_defaults(&s->users[0], s->next_id++, "Rider");
        s->n_users = 1;
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
    cJSON_AddStringToObject(root, "user", s->users[s->user].id);
    cJSON_AddNumberToObject(root, "next_user_id", s->next_id);
    cJSON *users = cJSON_AddArrayToObject(root, "users");
    for (int i = 0; i < s->n_users; i++) {
        const user_t *u = &s->users[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", u->id);
        cJSON_AddStringToObject(o, "name", u->name);
        cJSON_AddNumberToObject(o, "ftp_w", u->ftp_w);
        cJSON_AddNumberToObject(o, "max_erg_w", u->max_erg_w);
        cJSON_AddNumberToObject(o, "weight_kg", u->weight_kg);
        cJSON_AddNumberToObject(o, "max_hr", u->max_hr);
        cJSON_AddItemToArray(users, o);
    }
    cJSON_AddNumberToObject(root, "crr", s->crr / 10000.0);
    cJSON_AddNumberToObject(root, "cw", s->cw / 100.0);
    cJSON_AddStringToObject(root, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", s->wifi_pass);
    cJSON_AddStringToObject(root, "ap_password", s->ap_pass);
    cJSON_AddStringToObject(root, "tz", s->tz);
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
