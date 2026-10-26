#include "bsp.h"
#include "diag.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_ERROR_CHECK(bsp_init());

    // 7. Something visible before the backlight comes on; LVGL and BLE follow in M1.
    diag_test_pattern(0);
    bsp_backlight(true);

    ESP_LOGI(TAG, "ready, type 'help' on the USB console");
    diag_start();
}
