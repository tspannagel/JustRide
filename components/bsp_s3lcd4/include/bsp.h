// Board support for the Waveshare ESP32-S3-Touch-LCD-4 (V3).
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

// TCA9554 expander outputs
typedef enum {
    BSP_EXIO_TOUCH_RST = 0,
    BSP_EXIO_BACKLIGHT = 1,
    BSP_EXIO_LCD_RST = 2,
    BSP_EXIO_SD_CS = 3,
    BSP_EXIO_BUZZER = 5,
} bsp_exio_t;

// Bring-up steps 1-6 (I2C, expander, ST7701, RGB panel, SD). Backlight stays off.
// A missing SD card is logged, not treated as an error.
esp_err_t bsp_init(void);

// Probe every 7-bit address; writes up to max found addresses to out.
size_t bsp_i2c_scan(uint8_t *out, size_t max);

i2c_master_bus_handle_t bsp_i2c_bus(void);

esp_err_t bsp_exio_set(bsp_exio_t pin, bool level);
void bsp_backlight(bool on);

// Beep for ms (clamped to 10..1000). Returns immediately; a timer switches the
// buzzer off, so it can never be left on.
void bsp_beep(uint32_t ms);

// GT911 touch on the shared I2C bus. NULL if not found.
esp_lcd_touch_handle_t bsp_touch(void);

// PCF85063 real-time clock, UTC. bsp_rtc_get fails if the clock lost power.
esp_err_t bsp_rtc_get(time_t *t);
esp_err_t bsp_rtc_set(time_t t);

esp_lcd_panel_handle_t bsp_panel(void);
// One of the two RGB565 frame buffers in PSRAM (idx 0 or 1).
uint16_t *bsp_framebuffer(int idx);

bool bsp_sd_mounted(void);
