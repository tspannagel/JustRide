#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "ble_mgr.h"
#include "bsp.h"
#include "drv_zap.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "web.h"

#define SD_TEST_FILE BSP_SD_MOUNT "/diag.bin"
#define SD_TEST_KB 1024

void diag_test_pattern(int which)
{
    // RGB565: white, yellow, cyan, green, magenta, red, blue, black
    static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    uint16_t *fb = bsp_framebuffer();

    for (int y = 0; y < BSP_LCD_H; y++) {
        for (int x = 0; x < BSP_LCD_W; x++) {
            // Written straight into the frame buffer, so rotate by hand
            int vx = BSP_ROTATE_180 ? BSP_LCD_W - 1 - x : x;
            uint16_t c = which == 1 ? 0xFFFF : which == 2 ? 0x0000 : bars[vx * 8 / BSP_LCD_W];
            fb[y * BSP_LCD_W + x] = c;
        }
    }
    // Passing the frame buffer itself only flushes the cache, no copy.
    esp_lcd_panel_draw_bitmap(bsp_panel(), 0, 0, BSP_LCD_W, BSP_LCD_H, fb);
}

static int cmd_i2c(int argc, char **argv)
{
    uint8_t found[32];
    size_t n = bsp_i2c_scan(found, sizeof(found));
    // Expected: 0x24 CH32V003, 0x5D/0x14 GT911, 0x51 PCF85063
    for (size_t i = 0; i < n; i++) {
        printf("0x%02x\n", found[i]);
    }
    printf("%u device(s)\n", (unsigned)n);
    return 0;
}

static int cmd_pattern(int argc, char **argv)
{
    diag_test_pattern(argc > 1 ? atoi(argv[1]) : 0);
    return 0;
}

static int cmd_sd(int argc, char **argv)
{
    if (!bsp_sd_mounted()) {
        printf("SD not mounted\n");
        return 1;
    }
    static uint8_t buf[4096];
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }

    FILE *f = fopen(SD_TEST_FILE, "wb");
    if (!f) {
        printf("open for write failed\n");
        return 1;
    }
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < SD_TEST_KB / 4; i++) {
        fwrite(buf, 1, sizeof(buf), f);
    }
    fclose(f);
    int64_t t1 = esp_timer_get_time();

    f = fopen(SD_TEST_FILE, "rb");
    if (!f) {
        printf("open for read failed\n");
        return 1;
    }
    size_t total = 0, got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        total += got;
    }
    fclose(f);
    int64_t t2 = esp_timer_get_time();
    remove(SD_TEST_FILE);

    printf("write %d KB: %lld ms (%lld KB/s)\n", SD_TEST_KB, (t1 - t0) / 1000, SD_TEST_KB * 1000000LL / (t1 - t0));
    printf("read %u KB: %lld ms (%lld KB/s)\n", (unsigned)(total / 1024), (t2 - t1) / 1000,
           (long long)(total / 1024) * 1000000LL / (t2 - t1));
    return 0;
}

static int cmd_buzz(int argc, char **argv)
{
    bsp_beep(100);
    return 0;
}

static int cmd_bl(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: bl <0-255>\n");
        return 1;
    }
    int duty = atoi(argv[1]);
    duty = duty < 0 ? 0 : duty > 255 ? 255 : duty;
    return bsp_backlight_pwm((uint8_t)duty) == ESP_OK ? 0 : 1;
}

static int cmd_touch(int argc, char **argv)
{
    esp_lcd_touch_handle_t tp = bsp_touch();
    if (!tp) {
        printf("no touch controller\n");
        return 1;
    }
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    secs = secs < 1 ? 1 : secs > 120 ? 120 : secs;
    printf("touch the screen for %d s\n", secs);
    // Competes with LVGL for the same reads, so some points go to LVGL instead.
    int64_t end = esp_timer_get_time() + secs * 1000000LL;
    while (esp_timer_get_time() < end) {
        esp_lcd_touch_point_data_t pt;
        uint8_t n = 0;
        if (esp_lcd_touch_read_data(tp) == ESP_OK && esp_lcd_touch_get_data(tp, &pt, &n, 1) == ESP_OK && n) {
            printf("x %u y %u strength %u\n", pt.x, pt.y, pt.strength);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return 0;
}

// Read-only dump of the GT911 config header and status register, bypassing the driver.
static int cmd_gt911(int argc, char **argv)
{
    static const uint8_t addrs[] = {0x5D, 0x14};
    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (i2c_master_probe(bsp_i2c_bus(), addrs[i], 50) != ESP_OK) {
            continue;
        }
        i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000};
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bsp_i2c_bus(), &cfg, &dev) != ESP_OK) {
            continue;
        }
        uint8_t reg[2] = {0x80, 0x47}, c[8] = {0}, st = 0;
        esp_err_t e1 = i2c_master_transmit_receive(dev, reg, 2, c, sizeof(c), 100);
        reg[0] = 0x81;
        reg[1] = 0x4E;
        esp_err_t e2 = i2c_master_transmit_receive(dev, reg, 2, &st, 1, 100);
        printf("0x%02x: config %s ver %u x_max %u y_max %u points %u switch1 0x%02x | status %s 0x%02x\n", addrs[i],
               esp_err_to_name(e1), c[0], c[1] | c[2] << 8, c[3] | c[4] << 8, c[5], c[6], esp_err_to_name(e2), st);
        i2c_master_bus_rm_device(dev);
    }
    return 0;
}

static int cmd_ble(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "dump") == 0) {
        ble_mgr_dump(strcmp(argv[2], "on") == 0);
        return 0;
    }
    printf("usage: ble dump on|off\n");
    return 1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        app_web_start();  // only while no ride is running, like the Wi-Fi tile
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        web_stop();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        web_scan_print();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "allow") == 0) {
        web_allow_ota();  // as the "Allow firmware update" button: USB access is physical access
        printf("firmware and web app uploads allowed for 5 minutes\n");
        return 0;
    }
    printf("usage: wifi on|off|scan|allow\n");
    return 1;
}

static int cmd_zap(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "dump") == 0) {
        drv_zap_dump(strcmp(argv[2], "on") == 0);
        return 0;
    }
    printf("usage: zap dump on|off\n");
    return 1;
}

static int cmd_mem(int argc, char **argv)
{
    printf("internal free %u, PSRAM free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return 0;
}

void diag_start(void)
{
    static const esp_console_cmd_t cmds[] = {
        {.command = "i2c", .help = "Scan the I2C bus", .func = cmd_i2c},
        {.command = "pattern", .help = "Test pattern: 0 bars, 1 white, 2 black", .hint = "[0|1|2]", .func = cmd_pattern},
        {.command = "sd", .help = "SD write/read speed test (1 MB)", .func = cmd_sd},
        {.command = "buzz", .help = "Beep for 100 ms", .func = cmd_buzz},
        {.command = "bl", .help = "Backlight PWM duty (polarity unverified)", .hint = "<0-255>", .func = cmd_bl},
        {.command = "mem", .help = "Free heap and PSRAM", .func = cmd_mem},
        {.command = "touch", .help = "Print raw touch points", .hint = "[seconds]", .func = cmd_touch},
        {.command = "gt911", .help = "Dump GT911 config header and status", .func = cmd_gt911},
        {.command = "ble", .help = "Log every Bluetooth advertisement as hex", .hint = "dump on|off", .func = cmd_ble},
        {.command = "wifi", .help = "Wi-Fi on/off, scan, allow uploads (5 min)", .hint = "on|off|scan|allow", .func = cmd_wifi},
        {.command = "zap", .help = "Log Zwift Ride packets as hex", .hint = "dump on|off", .func = cmd_zap},
    };

    esp_console_repl_t *repl;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "jr>";
    esp_console_dev_usb_serial_jtag_config_t hw_cfg = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl));

    esp_console_register_help_command();
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
