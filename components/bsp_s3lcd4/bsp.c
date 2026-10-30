// Bring-up for the Waveshare ESP32-S3-Touch-LCD-4 V4.0.
// GPIO 1/2 are shared between the ST7701 init bus and the SD card, so the
// order in bsp_init() is mandatory (see CLAUDE.md "Bring-up order").
// Pins, CH32V003 registers, the ST7701 init table and the RGB timing are taken
// from Waveshare's V4 BSP (waveshare/esp32_s3_touch_lcd_4 3.0.0) and the
// waveshare/custom_io_expander_ch32v003 2.0.0 driver.

#include "bsp.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

#define PIN_I2C_SDA 15
#define PIN_I2C_SCL 7

#define PIN_LCD_CS 42
#define PIN_LCD_SCL 2
#define PIN_LCD_SDA 1
#define PIN_LCD_HSYNC 38
#define PIN_LCD_VSYNC 39
#define PIN_LCD_DE 40
#define PIN_LCD_PCLK 41

// 1-bit SDMMC
#define PIN_SD_CMD 1
#define PIN_SD_CLK 2
#define PIN_SD_D0 4

// CH32V003 helper controller: Waveshare firmware emulating an 8-bit expander
#define CH32_ADDR 0x24
#define CH32_REG_DIR 0x02 // 1 = output
#define CH32_REG_OUTPUT 0x03
#define CH32_REG_PWM 0x05 // backlight duty
// Pin table from the V4.0 schematic
#define CH32_TOUCH_RST 1
#define CH32_TOUCH_INT 2 // GT911 latches its I2C address and starts from INT's level at reset
#define CH32_LCD_RST 3
#define CH32_SD_CS 4 // card CS/D3: high selects SD mode at power-up (low = SPI mode)
#define CH32_SYS_EN 5 // system power enable; Waveshare drives it high in every example
#define CH32_BUZZER 6
// EXIO7 is the RTC interrupt (input); EXIO0 is the CH32's ADC input and stays untouched.
#define CH32_DIR_RUN \
    ((1u << CH32_TOUCH_RST) | (1u << CH32_LCD_RST) | (1u << CH32_SD_CS) | (1u << CH32_SYS_EN) | (1u << CH32_BUZZER))
#define CH32_DIR_RESET (CH32_DIR_RUN | (1u << CH32_TOUCH_INT))

#define PCF85063_ADDR 0x51
#define PCF85063_REG_SECONDS 0x04

#define BEEP_MIN_MS 10
#define BEEP_MAX_MS 1000

static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_exio;
static uint8_t s_exio_out;
static SemaphoreHandle_t s_exio_lock;
static i2c_master_dev_handle_t s_rtc;
static esp_timer_handle_t s_beep_timer;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static sdmmc_card_t *s_card;

// ---- I2C + CH32V003 helper --------------------------------------------------

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

i2c_master_bus_handle_t bsp_i2c_bus(void) { return s_i2c; }

size_t bsp_i2c_scan(uint8_t *out, size_t max)
{
    size_t n = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_i2c, addr, 50) == ESP_OK && n < max) {
            out[n++] = addr;
        }
    }
    return n;
}

static esp_err_t exio_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_exio, buf, sizeof(buf), 100);
}

static esp_err_t exio_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, CH32_ADDR, 50), TAG, "CH32V003 not found at 0x24");
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH32_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_exio), TAG, "add CH32V003");
    s_exio_lock = xSemaphoreCreateMutex();

    // Output register first, then direction, so no pin glitches.
    // Power enabled, LCD and touch held in reset with touch INT low, buzzer off.
    s_exio_out = (1u << CH32_SYS_EN) | (1u << CH32_SD_CS);
    ESP_RETURN_ON_ERROR(exio_write(CH32_REG_OUTPUT, s_exio_out), TAG, "CH32 output");
    ESP_RETURN_ON_ERROR(exio_write(CH32_REG_DIR, CH32_DIR_RESET), TAG, "CH32 direction");
    ESP_LOGI(TAG, "CH32V003 at 0x%02x", CH32_ADDR);
    return ESP_OK;
}

static esp_err_t exio_set(int pin, bool level)
{
    xSemaphoreTake(s_exio_lock, portMAX_DELAY);
    if (level) {
        s_exio_out |= 1u << pin;
    } else {
        s_exio_out &= ~(1u << pin);
    }
    esp_err_t err = exio_write(CH32_REG_OUTPUT, s_exio_out);
    xSemaphoreGive(s_exio_lock);
    return err;
}

esp_err_t bsp_backlight_pwm(uint8_t duty)
{
    xSemaphoreTake(s_exio_lock, portMAX_DELAY);
    esp_err_t err = exio_write(CH32_REG_PWM, duty);
    xSemaphoreGive(s_exio_lock);
    return err;
}

static void beep_off(void *arg) { exio_set(CH32_BUZZER, 0); }

void bsp_beep(uint32_t ms)
{
    if (!s_beep_timer) {
        return;
    }
    ms = ms < BEEP_MIN_MS ? BEEP_MIN_MS : ms > BEEP_MAX_MS ? BEEP_MAX_MS : ms;
    esp_timer_stop(s_beep_timer);
    exio_set(CH32_BUZZER, 1);
    esp_timer_start_once(s_beep_timer, ms * 1000);
}

// ---- RTC --------------------------------------------------------------------

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

esp_err_t bsp_rtc_get(time_t *t)
{
    if (!s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t reg = PCF85063_REG_SECONDS, r[7];
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_rtc, &reg, 1, r, sizeof(r), 100), TAG, "RTC read");
    if (r[0] & 0x80) {
        return ESP_ERR_INVALID_STATE; // oscillator stopped: time is invalid
    }
    struct tm tm = {
        .tm_sec = bcd2bin(r[0] & 0x7F),
        .tm_min = bcd2bin(r[1] & 0x7F),
        .tm_hour = bcd2bin(r[2] & 0x3F),
        .tm_mday = bcd2bin(r[3] & 0x3F),
        .tm_mon = bcd2bin(r[5] & 0x1F) - 1,
        .tm_year = bcd2bin(r[6]) + 100,
    };
    *t = mktime(&tm); // TZ is UTC on this device
    return ESP_OK;
}

esp_err_t bsp_rtc_set(time_t t)
{
    if (!s_rtc) {
        return ESP_ERR_INVALID_STATE;
    }
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t buf[8] = {
        PCF85063_REG_SECONDS,
        bin2bcd(tm.tm_sec), // bit 7 = 0 clears the oscillator-stopped flag
        bin2bcd(tm.tm_min),
        bin2bcd(tm.tm_hour),
        bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday,
        bin2bcd(tm.tm_mon + 1),
        bin2bcd(tm.tm_year % 100),
    };
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), 100);
}

static void rtc_init(void)
{
    if (i2c_master_probe(s_i2c, PCF85063_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not found");
        return;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 400000,
    };
    i2c_master_bus_add_device(s_i2c, &dev, &s_rtc);
}

// ---- Touch ------------------------------------------------------------------

static void touch_init(void)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (i2c_master_probe(s_i2c, io_cfg.dev_addr, 50) != ESP_OK) {
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
        if (i2c_master_probe(s_i2c, io_cfg.dev_addr, 50) != ESP_OK) {
            ESP_LOGW(TAG, "GT911 not found");
            return;
        }
    }
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io;
    if (esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io) != ESP_OK) {
        return;
    }
    // Reset is on CH32 EXIO1 (already released); INT is not wired to the ESP32.
    esp_lcd_touch_config_t cfg = {
        .x_max = BSP_LCD_W,
        .y_max = BSP_LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {.mirror_x = BSP_ROTATE_180, .mirror_y = BSP_ROTATE_180},
    };
    if (esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_touch) != ESP_OK) {
        s_touch = NULL;
    }
}

esp_lcd_touch_handle_t bsp_touch(void) { return s_touch; }

// ---- LCD --------------------------------------------------------------------

// Waveshare's V4 init table (BSP 3.0.0); ends with sleep out, inversion on, display on.
static const st7701_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x3B, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x21, 0x08}, 2, 0},
    {0xCD, (uint8_t[]){0x08}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x11, 0x18, 0x0E, 0x11, 0x06, 0x07, 0x08, 0x07, 0x22, 0x04, 0x12, 0x0F, 0xAA, 0x31, 0x18}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x11, 0x19, 0x0E, 0x12, 0x07, 0x08, 0x08, 0x08, 0x22, 0x04, 0x11, 0x11, 0xA9, 0x32, 0x18}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x60}, 1, 0},
    {0xB1, (uint8_t[]){0x30}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x49}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 20},
    {0xE0, (uint8_t[]){0x00, 0x1B, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x08, 0xA0, 0x00, 0x00, 0x07, 0xA0, 0x00, 0x00, 0x00, 0x44, 0x44}, 11, 0},
    {0xE2, (uint8_t[]){0x11, 0x11, 0x44, 0x44, 0xED, 0xA0, 0x00, 0x00, 0xEC, 0xA0, 0x00, 0x00}, 12, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0A, 0xE9, 0xD8, 0xA0, 0x0C, 0xEB, 0xD8, 0xA0, 0x0E, 0xED, 0xD8, 0xA0, 0x10, 0xEF, 0xD8, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x11, 0x11}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x09, 0xE8, 0xD8, 0xA0, 0x0B, 0xEA, 0xD8, 0xA0, 0x0D, 0xEC, 0xD8, 0xA0, 0x0F, 0xEE, 0xD8, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0xE4, 0xE4, 0x88, 0x00, 0x40}, 7, 0},
    {0xEC, (uint8_t[]){0x3C, 0x00}, 2, 0},
    {0xED, (uint8_t[]){0xAB, 0x89, 0x76, 0x54, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x20, 0x45, 0x67, 0x98, 0xBA}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x3A, (uint8_t[]){0x66}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 0, 120},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static esp_err_t lcd_init(void)
{
    // ST7701 is configured over a bit-banged 3-wire SPI on 1/2/42. The driver
    // deletes that IO after init (enable_io_multiplex) so 1/2 are free for SD.
    spi_line_config_t line = {
        .cs_io_type = IO_TYPE_GPIO,
        .cs_gpio_num = PIN_LCD_CS,
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = PIN_LCD_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = PIN_LCD_SDA,
        .io_expander = NULL,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_cfg = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line, 0);
    esp_lcd_panel_io_handle_t io;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_cfg, &io), TAG, "3-wire SPI");

    esp_lcd_rgb_panel_config_t rgb_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = 1,
        .bounce_buffer_size_px = BSP_LCD_W * 10, // two of these in internal RAM
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .disp_gpio_num = GPIO_NUM_NC,
        // B1-B5, G0-G5, R1-R5
        .data_gpio_nums = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17},
        // 16 MHz PCLK, same as Waveshare's V4 BSP
        .timings = ST7701_480_480_PANEL_60HZ_RGB_TIMING(),
        .flags.fb_in_psram = 1,
    };
    st7701_vendor_config_t vendor = {
        .rgb_config = &rgb_cfg,
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags.enable_io_multiplex = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, // reset is on CH32 EXIO3
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_cfg, &s_panel), TAG, "ST7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");

    // Keep the ST7701 deselected forever so SD traffic on 1/2 is ignored.
    gpio_config_t cs = {.pin_bit_mask = 1ULL << PIN_LCD_CS, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&cs);
    gpio_set_level(PIN_LCD_CS, 1);

    void *fb;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb), TAG, "frame buffer");
    s_fb = fb;
    return ESP_OK;
}

// The RGB panel's interrupt (it refills the bounce buffers from PSRAM every few
// lines) lands on the core that creates the panel. Create it on core 1: core 0
// runs the Bluetooth controller and Wi-Fi, whose interrupts delayed the refill
// and showed as flickering lines once a watch connected.
typedef struct {
    TaskHandle_t caller;
    esp_err_t err;
} lcd_job_t;

static void lcd_init_task(void *arg)
{
    lcd_job_t *job = arg;
    job->err = lcd_init();
    xTaskNotifyGive(job->caller);
    vTaskDelete(NULL);
}

static esp_err_t lcd_init_core1(void)
{
    lcd_job_t job = {.caller = xTaskGetCurrentTaskHandle(), .err = ESP_FAIL};
    if (xTaskCreatePinnedToCore(lcd_init_task, "lcd_init", 4096, &job, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return job.err;
}

esp_lcd_panel_handle_t bsp_panel(void) { return s_panel; }
uint16_t *bsp_framebuffer(void) { return s_fb; }

// ---- SD ---------------------------------------------------------------------

static esp_err_t sd_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP; // the board is wired for SPI: no CMD/D0 pull-ups

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT, &host, &slot, &mount, &s_card);
}

bool bsp_sd_mounted(void) { return s_card != NULL; }

// ---- Bring-up ---------------------------------------------------------------

esp_err_t bsp_init(void)
{
    // 1. I2C + CH32V003 helper
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(exio_init(), TAG, "CH32V003");
    esp_timer_create_args_t beep = {.callback = beep_off, .name = "beep"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&beep, &s_beep_timer), TAG, "beep timer");
    rtc_init();

    // 2. V4 has no SW6106; power is handled by the CH32 (SYS_EN set above).

    // 3. Resets were asserted by exio_init(); release them (Waveshare: 200 ms each).
    //    GT911 datasheet: INT low while RST rises selects address 0x5D; hold it
    //    > 50 ms, then release INT so the GT911 can drive it.
    vTaskDelay(pdMS_TO_TICKS(200));
    exio_set(CH32_LCD_RST, 1);
    exio_set(CH32_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exio_write(CH32_REG_DIR, CH32_DIR_RUN), TAG, "CH32 direction");
    vTaskDelay(pdMS_TO_TICKS(100));

    // 4 + 5. ST7701 init, RGB panel
    ESP_RETURN_ON_ERROR(lcd_init_core1(), TAG, "LCD");

    // Touch after the panel, like Waveshare's BSP: the GT911 needs time after reset.
    uint8_t found[16];
    size_t n = bsp_i2c_scan(found, sizeof(found));
    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "I2C device at 0x%02x", found[i]);
    }
    touch_init();

    // 6. SD (SDMMC) on the now-free 1/2/4
    esp_err_t err = sd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD not mounted: %s", esp_err_to_name(err));
        s_card = NULL;
    } else {
        ESP_LOGI(TAG, "SD mounted at " BSP_SD_MOUNT);
    }
    return ESP_OK;
}
