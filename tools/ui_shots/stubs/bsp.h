// Screenshot tool: stand-in for components/bsp_s3lcd4/include/bsp.h
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define BSP_LCD_W 480
#define BSP_LCD_H 480
#define BSP_ROTATE_180 1
#ifndef BSP_SD_MOUNT
#error "set BSP_SD_MOUNT"
#endif
static inline void *bsp_panel(void) { return 0; }
static inline void *bsp_touch(void) { return 0; }
static inline bool bsp_sd_space(uint64_t *total, uint64_t *free)
{
    *total = 31914983424ULL;  // a 32 GB card
    *free = 30872141824ULL;
    return true;
}
