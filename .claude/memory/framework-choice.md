---
name: framework-choice
description: Firmware framework is ESP-IDF >= 5.3 (C11), confirmed by the user; no Arduino
metadata:
  type: project
---

User chose ESP-IDF on 2026-10-26 (they had loosely called the project "ESP32/Arduino" before). Min version 5.3 because the BSP uses the `i2c_master` driver and `CONFIG_SPIRAM_XIP_FROM_PSRAM`.

**Why:** Plan's recommendation; best control over RGB panel + PSRAM and NimBLE multi-link.
**How to apply:** Don't reopen this. The .gitignore still has Arduino/PlatformIO entries; harmless ([[gitignore-conventions]]).
