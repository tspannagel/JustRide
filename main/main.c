#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include "app.h"
#include "ble_mgr.h"
#include "bsp.h"
#include "diag.h"
#include "drv_ftms.h"
#include "drv_hr.h"
#include "drv_zap.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "evbus.h"
#include "nvs_flash.h"
#include "ui.h"
#include "web.h"

static const char *TAG = "main";

static const ble_drv_t *const s_drivers[] = {&drv_ftms, &drv_hr, &drv_zap};

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static void clock_init(void)
{
    setenv("TZ", "UTC0", 1);  // everything on the device is UTC
    tzset();
    time_t t;
    if (bsp_rtc_get(&t) == ESP_OK) {
        struct timeval tv = {.tv_sec = t};
        settimeofday(&tv, NULL);
    } else {
        ESP_LOGW(TAG, "RTC not set; connect Wi-Fi once to set the clock");
    }
}

void app_main(void)
{
    nvs_init();
    ESP_ERROR_CHECK(bsp_init());
    clock_init();

    // 7. Black until LVGL draws (the backlight is on from power-up)
    diag_test_pattern(2);

    evbus_init();
    drv_ftms_init();  // before app_start: the control task feeds its mailbox
    app_start();
    ui_start();
    web_init();
    ble_mgr_start(s_drivers, sizeof(s_drivers) / sizeof(s_drivers[0]));

    // We got this far: keep this firmware (cancels OTA rollback)
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "ready, type 'help' on the USB console");
    diag_start();
}
