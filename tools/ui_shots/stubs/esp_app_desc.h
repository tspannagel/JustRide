#pragma once
typedef struct { char version[32]; } esp_app_desc_t;
static inline const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = {"v0.1.0"};
    return &d;
}
