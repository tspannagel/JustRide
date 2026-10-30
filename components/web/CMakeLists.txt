idf_component_register(
    SRCS "web.c"
    INCLUDE_DIRS "include"
    REQUIRES settings
    PRIV_REQUIRES bsp_s3lcd4 workout ics esp_wifi esp_netif esp_event esp_http_server app_update esp_timer esp_app_format bootloader_support json
)
