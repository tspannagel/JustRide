idf_component_register(
    SRCS "main.c" "app.c" "ui.c" "diag.c" "font_digits_100.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp_s3lcd4 evbus ble_mgr drv_ftms drv_hr drv_zap input_map ride recorder settings tz web
                  console esp_lcd esp_timer nvs_flash app_update esp_app_format
)
