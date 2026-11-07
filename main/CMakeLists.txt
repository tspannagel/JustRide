idf_component_register(
    SRCS "main.c" "app.c" "ui.c" "diag.c" "lv_mem_psram.c" "font_digits_100.c" "font_icons_18.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp_s3lcd4 evbus ble_mgr ble_srv drv_ftms drv_hr drv_zap input_map ride recorder route settings tz web
                  console esp_lcd esp_timer nvs_flash app_update esp_app_format
)

# LVGL's allocator lives here (lv_mem_psram.c); liblvgl comes after libmain on the link line
foreach(sym lv_mem_init lv_mem_deinit lv_malloc_core lv_realloc_core lv_free_core)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ${sym}")
endforeach()
