// Bring-up for the Waveshare ESP32-S3-Touch-LCD-4 (V3).
// GPIO 1/2 are shared between the ST7701 init bus and the SD card, so the
// order in bsp_init() is mandatory (see CLAUDE.md "Bring-up order").

#include "bsp.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
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
#define PIN_TOUCH_INT 16

#define PIN_LCD_CS 42
#define PIN_LCD_SCL 2
#define PIN_LCD_SDA 1
#define PIN_LCD_HSYNC 38
#define PIN_LCD_VSYNC 39
#define PIN_LCD_DE 40
#define PIN_LCD_PCLK 41

#define PIN_SD_MOSI 1
#define PIN_SD_SCK 2
#define PIN_SD_MISO 4

// TODO verify: Waveshare benchmark runs 21 MHz; start lower until the image is stable.
#define LCD_PCLK_HZ (16 * 1000 * 1000)

#define TCA9554_ADDR 0x20
#define TCA9554_ADDR_A 0x38 // TCA9554A variant
#define TCA9554_REG_OUTPUT 0x01
#define TCA9554_REG_CONFIG 0x03
// 1 = input. EXIO4 (BLC, unknown), EXIO6 (RTC int), EXIO7 (DO1, unknown) stay inputs.
#define TCA9554_CONFIG 0xD0

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
static uint16_t *s_fb[2];
static sdmmc_card_t *s_card;

// ---- I2C + expander ---------------------------------------------------------

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
    uint8_t addr = TCA9554_ADDR;
    if (i2c_master_probe(s_i2c, addr, 50) != ESP_OK) {
        addr = TCA9554_ADDR_A;
        ESP_RETURN_ON_ERROR(i2c_master_probe(s_i2c, addr, 50), TAG, "TCA9554 not found at 0x20/0x38");
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &dev, &s_exio), TAG, "add TCA9554");
    s_exio_lock = xSemaphoreCreateMutex();

    // Output register first, then direction, so no pin glitches high.
    // All outputs low: backlight off, resets asserted, SD CS low, buzzer off.
    s_exio_out = 0x00;
    ESP_RETURN_ON_ERROR(exio_write(TCA9554_REG_OUTPUT, s_exio_out), TAG, "TCA9554 output");
    ESP_RETURN_ON_ERROR(exio_write(TCA9554_REG_CONFIG, TCA9554_CONFIG), TAG, "TCA9554 config");
    ESP_LOGI(TAG, "TCA9554 at 0x%02x", addr);
    return ESP_OK;
}

esp_err_t bsp_exio_set(bsp_exio_t pin, bool level)
{
    xSemaphoreTake(s_exio_lock, portMAX_DELAY);
    if (level) {
        s_exio_out |= 1u << pin;
    } else {
        s_exio_out &= ~(1u << pin);
    }
    esp_err_t err = exio_write(TCA9554_REG_OUTPUT, s_exio_out);
    xSemaphoreGive(s_exio_lock);
    return err;
}

void bsp_backlight(bool on) { bsp_exio_set(BSP_EXIO_BACKLIGHT, on); }

static void beep_off(void *arg) { bsp_exio_set(BSP_EXIO_BUZZER, 0); }

void bsp_beep(uint32_t ms)
{
    if (!s_beep_timer) {
        return;
    }
    ms = ms < BEEP_MIN_MS ? BEEP_MIN_MS : ms > BEEP_MAX_MS ? BEEP_MAX_MS : ms;
    esp_timer_stop(s_beep_timer);
    bsp_exio_set(BSP_EXIO_BUZZER, 1);
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
    // Reset is on EXIO0 (already released); INT is only read, never driven.
    esp_lcd_touch_config_t cfg = {
        .x_max = BSP_LCD_W,
        .y_max = BSP_LCD_H,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    if (esp_lcd_touch_new_i2c_gt911(io, &cfg, &s_touch) != ESP_OK) {
        s_touch = NULL;
    }
}

esp_lcd_touch_handle_t bsp_touch(void) { return s_touch; }

// ---- LCD --------------------------------------------------------------------

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
        .num_fbs = 2,
        .bounce_buffer_size_px = BSP_LCD_W * 10,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .disp_gpio_num = GPIO_NUM_NC,
        // B1-B5, G0-G5, R1-R5
        .data_gpio_nums = {5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17},
        // TODO verify porches against the Waveshare demo if the image is shifted.
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BSP_LCD_W,
            .v_res = BSP_LCD_H,
            .hsync_pulse_width = 10,
            .hsync_back_porch = 10,
            .hsync_front_porch = 20,
            .vsync_pulse_width = 10,
            .vsync_back_porch = 10,
            .vsync_front_porch = 10,
        },
        .flags.fb_in_psram = 1,
    };
    // TODO verify: uses the driver's default ST7701 init sequence. If colours or
    // gamma look wrong, copy the init_cmds table from Waveshare's demo here.
    st7701_vendor_config_t vendor = {
        .rgb_config = &rgb_cfg,
        .flags.enable_io_multiplex = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC, // reset is on EXIO2
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

    void *fb0, *fb1;
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &fb0, &fb1), TAG, "frame buffers");
    s_fb[0] = fb0;
    s_fb[1] = fb1;
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_panel(void) { return s_panel; }
uint16_t *bsp_framebuffer(int idx) { return s_fb[idx & 1]; }

// ---- SD ---------------------------------------------------------------------

static esp_err_t sd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .sclk_io_num = PIN_SD_SCK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SD SPI bus");

    // CS is EXIO3, held low by exio_init(); the SPI driver gets no CS pin.
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdspi_mount(BSP_SD_MOUNT, &host, &slot, &mount, &s_card);
}

bool bsp_sd_mounted(void) { return s_card != NULL; }

// ---- Bring-up ---------------------------------------------------------------

esp_err_t bsp_init(void)
{
    // 1. I2C + expander
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(exio_init(), TAG, "expander");
    esp_timer_create_args_t beep = {.callback = beep_off, .name = "beep"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&beep, &s_beep_timer), TAG, "beep timer");
    rtc_init();

    // 2. TODO SW6106 light-load fix (Waveshare FAQ). Deliberately not done:
    //    never write power-chip registers that aren't confirmed from the datasheet/FAQ.

    // 3. Resets were asserted by exio_init(); release them.
    vTaskDelay(pdMS_TO_TICKS(20));
    bsp_exio_set(BSP_EXIO_LCD_RST, 1);
    bsp_exio_set(BSP_EXIO_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    uint8_t found[16];
    size_t n = bsp_i2c_scan(found, sizeof(found));
    for (size_t i = 0; i < n; i++) {
        ESP_LOGI(TAG, "I2C device at 0x%02x", found[i]);
    }
    touch_init();

    // 4 + 5. ST7701 init, RGB panel
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "LCD");

    // 6. SD on the now-free 1/2/4
    esp_err_t err = sd_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD not mounted: %s", esp_err_to_name(err));
        s_card = NULL;
    } else {
        ESP_LOGI(TAG, "SD mounted at " BSP_SD_MOUNT);
    }
    return ESP_OK;
}
