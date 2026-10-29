// Touch UI on the 480x480 panel (LVGL 9 via esp_lvgl_port).
#pragma once

void ui_start(void);

// Controller menu key (a nav_key_t). Safe to call from any one task; handled in the LVGL task.
void ui_nav(int key);
