#include "diag.h"

#include <stdio.h>
#include <stdlib.h>
#include "bsp.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"

#define SD_TEST_FILE BSP_SD_MOUNT "/diag.bin"
#define SD_TEST_KB 1024

void diag_test_pattern(int which)
{
    // RGB565: white, yellow, cyan, green, magenta, red, blue, black
    static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    uint16_t *fb = bsp_framebuffer(0);

    for (int y = 0; y < BSP_LCD_H; y++) {
        for (int x = 0; x < BSP_LCD_W; x++) {
            uint16_t c = which == 1 ? 0xFFFF : which == 2 ? 0x0000 : bars[x * 8 / BSP_LCD_W];
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
    // Expected: 0x20/0x38 TCA9554, 0x5D/0x14 GT911, 0x51 PCF85063, SW6106 unknown
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
        {.command = "mem", .help = "Free heap and PSRAM", .func = cmd_mem},
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
