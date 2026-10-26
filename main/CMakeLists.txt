idf_component_register(
    SRCS "main.c" "diag.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp_s3lcd4 console esp_lcd esp_timer
)
