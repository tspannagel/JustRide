idf_component_register(
    SRCS "bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd driver
    PRIV_REQUIRES esp_timer fatfs sdmmc
)
