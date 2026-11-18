// The web app inside the firmware (tools/pack_www.py, without vendor/) goes onto the SD card
// on the first start of a new firmware version, so a USB flash or an OTA update brings the
// matching web app along. fw-installed.txt remembers the firmware build (ELF hash, so two
// "-dirty" builds with the same version differ) that did it: a newer web app sent with
// tools/web_deploy.sh afterwards stays until the next firmware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "web.h"
#include "wwwpack.h"

static const char *TAG = "www";

extern const uint8_t www_pack_start[] asm("_binary_www_pack_start");
extern const uint8_t www_pack_end[] asm("_binary_www_pack_end");

#define DIR BSP_SD_MOUNT "/www/stats"
#define MARK DIR "/fw-installed.txt"
#define MANIFEST DIR "/deploy-manifest.txt"
#define MANIFEST_MAX (32 * 1024)

static void *ext_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

// Via a temp file: a write cut short (power off) never replaces a good file
static bool write_file(const char *path, const void *data, size_t n)
{
    char tmp[140];
    snprintf(tmp, sizeof(tmp), "%.128s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(path);  // FAT: rename doesn't replace
        ok = rename(tmp, path) == 0;
    } else {
        remove(tmp);
    }
    return ok;
}

static void mkdirs(char *path)
{
    for (char *p = path + strlen(BSP_SD_MOUNT) + 1; (p = strchr(p, '/')); p++) {
        *p = 0;
        mkdir(path, 0775);
        *p = '/';
    }
}

static size_t read_small(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    buf[n] = 0;
    return n;
}

static bool install(const char *ver, const char *build)
{
    size_t len = (size_t)(www_pack_end - www_pack_start);
    int count = wwwpack_check(www_pack_start, len);
    if (count < 0) {
        ESP_LOGE(TAG, "web app pack damaged, not installed");
        return false;
    }
    ESP_LOGI(TAG, "installing web app %s (%d files, %u KiB)", ver, count, (unsigned)(len / 1024));
    int64_t t0 = esp_log_timestamp();
    char **lines = ext_malloc(sizeof(char *) * (size_t)(count + 1));
    char *line_buf = ext_malloc((size_t)(count + 1) * 140);
    char *old = ext_malloc(MANIFEST_MAX), *out = ext_malloc(MANIFEST_MAX);
    bool ok = lines && line_buf && old && out;
    int n = 0;
    size_t off = 0;
    wwwpack_entry_t e;
    char path[140];
    mkdir(BSP_SD_MOUNT "/www", 0775);
    mkdir(DIR, 0775);
    while (ok && wwwpack_next(www_pack_start, len, &off, &e)) {
        snprintf(path, sizeof(path), DIR "/%s", e.name);
        mkdirs(path);
        if (!write_file(path, e.data, e.size)) {
            ESP_LOGE(TAG, "%s: write failed", e.name);
            ok = false;
        }
        lines[n] = line_buf + (size_t)n * 140;
        snprintf(lines[n], 140, "%s  %s", e.sha, e.plain);
        n++;
    }
    if (ok) {  // version.txt as tools/web_deploy.sh writes it, and its manifest line
        char v[40];
        int vn = snprintf(v, sizeof(v), "%.31s\n", ver);
        uint8_t sha[32];
        mbedtls_sha256((const unsigned char *)v, (size_t)vn, sha, 0);
        lines[n] = line_buf + (size_t)n * 140;
        for (int i = 0; i < 32; i++) {
            sprintf(lines[n] + 2 * i, "%02x", sha[i]);
        }
        strcpy(lines[n] + 64, "  version.txt");
        n++;
        ok = write_file(DIR "/version.txt", v, (size_t)vn);
    }
    if (ok) {  // web_deploy.sh --check then knows the device is current; DuckDB lines stay
        read_small(MANIFEST, old, MANIFEST_MAX);
        size_t m = wwwpack_manifest(old, lines, n, out, MANIFEST_MAX);
        ok = m > 0 && write_file(MANIFEST, out, m);
    }
    if (ok) {
        char mark[40];
        int mn = snprintf(mark, sizeof(mark), "%.16s\n", build);
        ok = write_file(MARK, mark, (size_t)mn);
    }
    heap_caps_free(lines);
    heap_caps_free(line_buf);
    heap_caps_free(old);
    heap_caps_free(out);
    if (ok) {
        ESP_LOGI(TAG, "web app installed in %u ms", (unsigned)(esp_log_timestamp() - t0));
    } else {
        ESP_LOGE(TAG, "web app install failed: tried again on the next start");
    }
    return ok;
}

static TaskHandle_t s_waiter;

static char s_build[17];

static void install_task(void *arg)
{
    install(arg, s_build);
    xTaskNotifyGive(s_waiter);
    vTaskDelete(NULL);
}

void web_install_app(void)
{
    if (!bsp_sd_mounted()) {
        return;
    }
    const char *ver = esp_app_get_description()->version;
    esp_app_get_elf_sha256(s_build, sizeof(s_build));
    char mark[40];
    read_small(MARK, mark, sizeof(mark));
    mark[strcspn(mark, "\r\n")] = 0;
    if (strcmp(mark, s_build) == 0) {
        return;
    }
    // Own task: app_main's stack is too small for FATFS + this. Waited for, so its stack is
    // back before Wi-Fi and Bluetooth take the internal RAM; once per firmware version (~2 s).
    s_waiter = xTaskGetCurrentTaskHandle();
    if (xTaskCreatePinnedToCore(install_task, "www_inst", 6144, (void *)ver, 3, NULL, 1) == pdPASS) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}
