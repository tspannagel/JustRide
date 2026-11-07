// LVGL's allocator (LV_USE_CUSTOM_MALLOC): objects, styles and draw tasks in PSRAM.
// With the C library's malloc every allocation up to CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// (16 KB) went to internal RAM first, and the UI's few hundred objects took the internal
// DMA memory that the SD card needs for every sector read (512 B bounce buffers).
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, LV_CAPS); }

void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc(p, new_size, LV_CAPS); }

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
