// Screenshot tool: just enough of esp_lvgl_port for ui_start() to compile (it is never called).
#pragma once
#include <stdbool.h>
#include "lvgl.h"
typedef int esp_err_t;
#define ESP_ERROR_CHECK(x) ((void)(x))
typedef struct { int task_affinity, task_stack; } lvgl_port_cfg_t;
#define ESP_LVGL_PORT_INIT_CONFIG() {0}
typedef struct {
    void *panel_handle;
    unsigned buffer_size, hres, vres;
    bool double_buffer;
    lv_color_format_t color_format;
    struct { bool swap_xy, mirror_x, mirror_y; } rotation;
    struct { bool buff_spiram, direct_mode; } flags;
} lvgl_port_display_cfg_t;
typedef struct { struct { bool bb_mode, avoid_tearing; } flags; } lvgl_port_display_rgb_cfg_t;
typedef struct { lv_display_t *disp; void *handle; } lvgl_port_touch_cfg_t;
static inline esp_err_t lvgl_port_init(const lvgl_port_cfg_t *c) { return 0; }
static inline lv_display_t *lvgl_port_add_disp_rgb(const lvgl_port_display_cfg_t *d, const lvgl_port_display_rgb_cfg_t *r) { return 0; }
static inline void *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *t) { return 0; }
static inline bool lvgl_port_lock(int t) { return true; }
static inline void lvgl_port_unlock(void) {}
