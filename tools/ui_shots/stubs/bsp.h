// Screenshot tool: stand-in for components/bsp_s3lcd4/include/bsp.h
#pragma once
#define BSP_LCD_W 480
#define BSP_LCD_H 480
#ifndef BSP_SD_MOUNT
#error "set BSP_SD_MOUNT"
#endif
static inline void *bsp_panel(void) { return 0; }
static inline void *bsp_touch(void) { return 0; }
