---
name: no-local-idf
description: As of 2026-10-26 the dev Mac has no ESP-IDF and no cmake; all firmware is unbuilt, host tests run with plain cc
metadata:
  type: project
---

On 2026-10-26 `idf.py` and `cmake` were not installed. Firmware code was written without compiling; host tests were verified with
`cc -std=c11 -Wall -Wextra -Werror -Icomponents/proto_ftms/include test/host/test_proto_ftms.c components/proto_ftms/proto_ftms.c`.

Off-device checks that do work here: host tests via cc + ASan/UBSan; FIT validation with fitdecode in a scratch venv; the stats app in headless Chrome via puppeteer-core with a local server mimicking the device endpoints.

**Why:** Explains why the first `idf.py build` may surface compile errors (esp_lcd_st7701, esp_lvgl_port, gt911, NimBLE API details were written from memory).
**How to apply:** Check `which idf.py cmake` before claiming a build works; delete this entry once the toolchain is installed and the firmware has built.
