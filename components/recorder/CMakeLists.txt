idf_component_register(
    SRCS "csv.c" "fit.c" "recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES evbus metrics
    PRIV_REQUIRES esp_app_format
)
