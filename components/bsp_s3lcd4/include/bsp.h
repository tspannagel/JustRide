// Board support for the Waveshare ESP32-S3-Touch-LCD-4 V4.0.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

#define BSP_LCD_W 480
#define BSP_LCD_H 480
#define BSP_SD_MOUNT "/sdcard"
// The panel's native top is at the USB-C side; mounted with the power button at the
// bottom right, the picture and touch are turned 180 degrees.
#define BSP_ROTATE_180 1

// Bring-up steps 1-6 (I2C, CH32V003 helper, ST7701, RGB panel, SD).
// The backlight is left at the CH32's power-on default (on).
// A missing SD card is logged, not treated as an error.
esp_err_t bsp_init(void);

// Probe every 7-bit address; writes up to max found addresses to out.
size_t bsp_i2c_scan(uint8_t *out, size_t max);

i2c_master_bus_handle_t bsp_i2c_bus(void);

// Backlight PWM duty via the CH32. Polarity unverified: Waveshare's examples
// disagree (255 = full in one, 0 = full in another). Only used by `diag bl` for now.
esp_err_t bsp_backlight_pwm(uint8_t duty);

// Beep for ms (clamped to 10..1000). Returns immediately; a timer switches the
// buzzer off, so it can never be left on.
void bsp_beep(uint32_t ms);

// GT911 touch on the shared I2C bus. NULL if not found.
esp_lcd_touch_handle_t bsp_touch(void);

// PCF85063 real-time clock, UTC. bsp_rtc_get fails if the clock lost power.
esp_err_t bsp_rtc_get(time_t *t);
esp_err_t bsp_rtc_set(time_t t);

esp_lcd_panel_handle_t bsp_panel(void);
// The panel's RGB565 frame buffer in PSRAM.
uint16_t *bsp_framebuffer(void);

bool bsp_sd_mounted(void);
// Card size and free space in bytes. False if no card is mounted.
bool bsp_sd_space(uint64_t *total, uint64_t *free);

// One folder entry from bsp_sd_list. mtime is what stat() gives for the same file.
typedef struct {
    const char *name;
    uint32_t size;
    int64_t mtime;
    bool dir;
} bsp_dirent_t;

// Every entry of a folder below BSP_SD_MOUNT (e.g. "rides/u1"), with size and time from
// the folder itself: one pass over the folder instead of a stat() per file, which on FAT
// searches the whole folder again each time. cb returns false to stop. False if the folder
// can't be opened.
bool bsp_sd_list(const char *rel_dir, bool (*cb)(const bsp_dirent_t *e, void *ctx), void *ctx);
