# KICKR Head Unit

Standalone ESP32-S3 head unit for indoor cycling. It controls a Wahoo KICKR Core over Bluetooth (FTMS), reads all Zwift controllers (Click v1/v2, Play, Ride), runs structured workouts from microSD, records rides locally (FIT + CSV) and shows live stats on a 4" touchscreen. No cloud, no 3D world. A browser web app (statistics, workout builder/generator, training plans) is served by the device itself over Wi-Fi.

Full plan with diagrams: `docs/plan.html` (open in a browser; Mermaid diagrams may not render offline). `HANDOVER.md` has current status, decisions and open questions.

## Hardware

**Board: Waveshare ESP32-S3-Touch-LCD-4, V4.0** (the user's board says REV 4; confirmed 2026-10-28). Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4 (official STEP model under Resources). V4 source of truth: github.com/waveshareteam/ESP32-S3-Touch-LCD-4 (`hardware/` schematic + reference) and the managed components `waveshare/esp32_s3_touch_lcd_4` 3.0.0 / `waveshare/custom_io_expander_ch32v003` 2.0.0. V3 (TCA9554, SW6106, SD over SPI) is not supported anymore.

- ESP32-S3-N16R8: 16 MB flash, 8 MB octal PSRAM, BLE 5, Wi-Fi 2.4 GHz
- 4" IPS 480×480, ST7701, 16-bit RGB565 parallel bus (frame buffers in PSRAM)
- GT911 capacitive touch (answers at 0x14), PCF85063 RTC (0x51), CH32V003 helper MCU at 0x24 (expander + backlight PWM + battery ADC; replaces V3's TCA9554 and SW6106)
- microSD over 1-bit SDMMC, onboard buzzer, USB-C (native USB: flashing + console)
- RS485, CAN, 7–36 V terminal: present, unused
- **No free GPIOs.** Do not plan features that need extra wires.

### Pin map (V4, from Waveshare's V4 BSP)

| Function | GPIO |
|---|---|
| LCD R1–R5 | 46, 3, 8, 18, 17 |
| LCD G0–G5 | 14, 13, 12, 11, 10, 9 |
| LCD B1–B5 | 5, 45, 48, 47, 21 |
| LCD HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 (PCLK 16 MHz, as in Waveshare's V4 BSP) |
| ST7701 init 3-wire SPI SDA / SCL / CS | 1 / 2 / 42 |
| SD (SDMMC 1-bit) CMD / CLK / D0 | 1 / 2 / 4 (shares 1 and 2 with LCD init) |
| I2C SDA / SCL | 15 / 7 (touch, RTC, CH32V003) |
| Touch INT / RST | not on a GPIO / CH32 EXIO1 |
| USB D− / D+ | 19 / 20 |
| RS485 RX / TX (unused) | 43 / 44 |
| CAN RX / TX (unused) | 0 / 6 (GPIO0 is also BOOT) |

CH32V003 registers (Waveshare driver): 0x02 direction (1 = output), 0x03 output, 0x04 input, 0x05 backlight PWM, 0x06 ADC (battery, 10 bit), 0x07 RTC int. Pins (V4.0 schematic pin table): EXIO0 CH32 ADC (untouched), EXIO1 touch reset, EXIO2 touch INT, EXIO3 LCD reset, EXIO4 SD CS/D3 (high), EXIO5 SYS_EN (power enable, high like Waveshare), EXIO6 buzzer, EXIO7 RTC interrupt (input). Waveshare's BSP doesn't name EXIO2/EXIO4 but its driver drives every pin low; with EXIO2 floating the GT911 boots with an empty config (0×0, no touches). The backlight is on at power-up; PWM polarity is unverified (Waveshare's examples disagree), so the firmware doesn't write it except via `diag bl`.

### Bring-up order (shared pins make this mandatory)

1. I2C on 15/7, probe the CH32V003 (0x24) and PCF85063
2. CH32: SYS_EN + SD CS high, LCD + touch reset low, touch INT low, buzzer off (output register before direction); 200 ms, release resets, 100 ms, release touch INT (input), 100 ms
3. ST7701 init over 3-wire SPI (1/2/42) with Waveshare's V4 init table, IO deleted afterwards, then hold CS 42 high permanently
4. Start the RGB panel (`esp_lcd_panel_rgb`, 16 MHz), two 480×480 RGB565 frame buffers in PSRAM
5. Probe GT911 (after the panel: it needs time after reset)
6. Mount the SD card over SDMMC 1-bit on 1/2/4 with internal pull-ups (the board is wired for SPI and has none)
7. Start LVGL, then BLE, then the control loop

RGB + PSRAM: PSRAM 80 MHz octal, 10-line bounce buffers (two, in internal RAM), 64-byte data cache lines, `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA` on. One frame buffer, LVGL partial rendering (48 lines, PSRAM) like Waveshare's BSP: direct mode on two frame buffers showed a shifted, flickering second image. Internal RAM is tight (~39 KB free): a 64 KB data cache or 20-line bounce buffers broke BLE / the HTTP server. Wi-Fi traffic can make the RGB image drift; Wi-Fi is only on when not riding. Time: SNTP starts on got-IP with the router (DHCP gateway) as server 0 and pool.ntp.org as server 1; the user blocks the device from the internet, so the router or the web app (`/api/time`) sets the clock. `CONFIG_LCD_RGB_ISR_IRAM_SAFE` keeps the bounce-buffer ISR running while flash is busy (added after a lasting shift when Wi-Fi came back on after a ride; ~17 KB internal RAM left with Wi-Fi + BLE). Big static buffers (`s_wo` in app.c, `s_preview` in ui.c) live in PSRAM via `EXT_RAM_BSS_ATTR` (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`), ~29 KB internal free at idle. The RGB panel is created from a task pinned to core 1 so its bounce-buffer ISR doesn't share core 0 with the BT controller and Wi-Fi (a connected Apple Watch at 30 ms caused flickering lines); `ble_srv` also asks receivers for a 90–120 ms link (the watch accepts 118.75 ms) and advertises every 200–300 ms. No gradients on dark UI surfaces: RGB565 bands.

### Power safety

Use a plain 5 V USB-A supply and a USB-A to USB-C cable. Waveshare reports fast-charge sources (up to 20 V) burning protection parts on this board. Never suggest powering it from a laptop/phone fast charger. During development, the computer's USB port is fine.

## Firmware stack

- **ESP-IDF ≥ 5.3**, C (C11). Arduino is not used (confirmed by the user).
- NimBLE host as BLE central (sensors, up to 4 links, `MAX_LINKS` in `ble_mgr.c`) and peripheral (re-broadcast as "JustRide": Cycling Power 0x1818 + CSC 0x1816, up to 2 receivers, read-only)
- LVGL 9 via `esp_lvgl_port`, partial rendering into one RGB frame buffer, system malloc (`CONFIG_LV_USE_CLIB_MALLOC`)
- mbedTLS (ECDH P-256, HKDF-SHA256, AES-CCM) for encrypted Zwift controllers
- nanopb for Zwift protobuf messages (when ZAP is implemented)
- Own minimal FIT encoder (`recorder/fit.c`, ~300 lines) instead of the Garmin FIT SDK; validated with fitdecode
- FreeRTOS: core 0 = NimBLE host + Wi-Fi, core 1 = control loop, LVGL, recorder, storage

### Repo layout

```
main/                     app_main (bring-up order), app.c control task, ui.c LVGL screens (palette + style helpers at the top), font_digits_100.c (power digits) + font_icons_18.c (Font Awesome icons), both generated with lv_font_conv, diag.c console
components/
  bsp_s3lcd4/             board support: I2C, CH32V003, ST7701 init, RGB panel, SD, RTC, touch, buzzer
  evbus/                  jr_types.h (stable event/sample types, pure) + one FreeRTOS queue
  ble_mgr/                NimBLE central: scan, connect, GATT discovery, CCCD subscribe, back-off, role per device (NVS)
  ble_srv/                NimBLE peripheral: power + cadence re-broadcast for a watch/bike computer (notify only, no control)
  proto_cps/              pure: CPS/CSC measurement encoding, cadence -> crank revolutions model
  drv_ftms/               FTMS trainer driver: metrics out, control point with one write in flight
  drv_hr/                 heart-rate strap (0x180D)
  drv_zap/                Zwift Ride (plain ZAP): handshake, button edges -> input events, raw hex dump
  proto_ftms/             pure codec: Indoor Bike Data parsing, control point encode/decode; ftms_ctl.c control-point state machine
  proto_zap/              pure codec: RideOn handshake, 0x23 keypad protobuf, button table (verified on the Ride)
  input_map/              pure: input events -> ride actions by ride state (long press for stop/skip/start)
  workout/                pure: .zwo parser + workout timeline, adaptive goals (goal_t)
  vshift/                 pure: gear table, strategy-A grade scaling
  metrics/                pure: NP, IF, TSS, kJ, distance, best efforts
  ride/                   pure: ride state machine, trainer command, safety limits, auto-pause, lap/interval feedback
  route/                  pure: .route parser (line by line), elevation/gradient by distance (100 m window, clamped)
  recorder/               csv.c/fit.c pure writers; recorder.c SD files, journal, crash recovery
  settings/               settings.json, gears.json
  tz/                     pure: local time from a POSIX TZ rule (ride names, FIT local time), preset list
  web/                    Wi-Fi (STA, AP fallback), HTTP, mDNS, SNTP, OTA
  ics/                    pure: one iCalendar event (RFC 5545) for "Add to calendar"
www/stats/                web app: core.js (API, CSV, maths, charts), app.js (Today/Rides/Power/Device), workouts.js (model, .zwo, generator, editor), plan.js (plans + device sync), riders.js (rider management), routes.js (GPX -> .route); vendor/ generated by tools/vendor_stats.sh, not committed
test/host/                host unit tests (CMake + ctest, ASan/UBSan)
tools/                    sample .zwo files, vendor_stats.sh, sd_deploy.sh, web_deploy.sh (web app over Wi-Fi), web_dev.py (web app locally: samples or --device), ui_shots/ (renders main/ui.c on the host -> docs/img)
docs/                     plan.html (planning), manual.html (user manual: setup + operation), img/ (screenshots)
```

Not built yet: encrypted ZAP (Play, Click v1; the user owns a Zwift Ride only), cycling-power fallback, intervals.icu upload (skipped for now, user 2026-10-30), virtual shifting v2 (after the user has tried v1 on the bike).

## Architecture rules

- Drivers publish normalized events to the single evbus queue; the control task (`main/app.c`) consumes them. **Nothing above `evbus` touches a BLE characteristic.**
- Event types (keep these stable):
  - `metrics_evt_t`: t_ms, power_w (−1 unknown), cadence_rpm10, speed_kmh100, hr_bpm, source
  - `input_evt_t`: id (SHIFT_UP/DOWN, LEFT, RIGHT, UP, DOWN, A, B, Y, Z, STEER, BRAKE), kind (PRESS, RELEASE, LONG_PRESS, REPEAT, ANALOG), value, device
  - `control_cmd_t`: kind (ERG, SIM, RESISTANCE, START, PAUSE, STOP, SPINDOWN), target_w, grade (0.01 %), crr, cw, level
  - `device_evt_t`: connected / lost / battery / role
- Device drivers are a `ble_drv_t`: advertised service (= `matches(adv)`), wanted characteristics, `on_ready`, `on_notify`, `on_disconnect`; trainers add a `set(cmd)` mailbox (`drv_ftms_set`). ble_mgr does all GATT work.
- Codecs and parsers (`proto_*`, `zwo`, `metrics`, `vshift`, CSV/FIT writers) are pure C with no ESP-IDF dependencies so they run in host tests.
- Integers with fixed units on the bus; no floats in event structs.
- Trainer control point: one command in flight, wait for the 0x80 response (≤ 1 s timeout), max 4 writes/s, SIM grade updates max 2/s. After reconnect: Request Control, Start, resend current target. These rules live in the pure `ftms_ctl.c`; `drv_ftms.c` is only queues, BLE writes and logging.

## Protocol reference

### FTMS (KICKR Core)

KICKR Core estimated cadence is only sent via FTMS, not via Cycling Power. KICKR allows max 3 BLE connections.

| Item | UUID / code |
|---|---|
| Fitness Machine service | 0x1826 |
| Feature (read) | 0x2ACC |
| Indoor Bike Data (notify) | 0x2AD2 |
| Supported Resistance Range | 0x2AD6 |
| Supported Power Range | 0x2AD8 |
| Control Point (write, indicate) | 0x2AD9 |
| Machine Status (notify) | 0x2ADA |
| Cycling Power fallback | 0x1818 / 0x2A63 |
| Wahoo legacy control (only if no FTMS; verify) | A026E005-0A7D-4AB3-97FA-F1500F9FEB8B |

Control point opcodes: 0x00 Request Control, 0x01 Reset, 0x04 Target Resistance (uint8, 0.1), 0x05 Target Power (sint16 W), 0x07 Start/Resume, 0x08 Stop/Pause (1 stop, 2 pause), 0x11 Indoor Bike Simulation (wind sint16 0.001 m/s, grade sint16 0.01 %, Crr uint8 0.0001, Cw uint8 0.01 kg/m), 0x13 Spin Down, 0x80 Response (req op, result; 0x01 = success).

### Zwift Accessory Protocol (ZAP) — Ride verified 2026-10-29, others community reverse-engineered

**User's Zwift Ride (capture: `test/host/fixtures/zap_ride_2026-10-29.txt`):** adverts carry UUID16 0xFC82 + company 0x094A + name "Zwift Ride"; both controllers advertise (type byte 0x07 / 0x08) and all buttons of both arrive over one connection. The GATT service is **0xFC82**, not 00000001-…; the characteristics inside keep the 128-bit UUIDs below, plus an extra 00000006 (props 0x2e). Needs an MTU exchange (152 agreed). Keypad 0x23: field 1 = 32-bit button map, active low; bits 0–3 left/up/right/down, 4–7 A/B/Y/Z, 8/9 left back upper/lower, 12/13 right back upper/lower (the community table was one bit off from Z up). Paddles are analog (repeated field 3: index 0 left, 1 right, zigzag −100…100), not bits. Other async: 0x15 ~1/s, 0x19 likely battery.

| Item | UUID |
|---|---|
| Service | 00000001-19CA-4651-86E5-FA29DCDD09D1 (older firmware; the Ride uses 0xFC82) |
| Async (notify) | 00000002-19CA-4651-86E5-FA29DCDD09D1 |
| SyncRX (write) | 00000003-19CA-4651-86E5-FA29DCDD09D1 |
| SyncTX (indicate) | 00000004-19CA-4651-86E5-FA29DCDD09D1 |

- Plain (Ride, Click v2, newer firmware): write `"RideOn"` → reply `"RideOn"` → Async carries plain protobuf (Ride: message ID 0x23 with button bitmap + analog paddles).
- Encrypted (Play, Click v1): write `"RideOn"` + 64-byte P-256 public key → reply `"RideOn"` + device key → ECDH → HKDF-SHA256 → AES-CCM; Async = counter + ciphertext + tag.
- Driver tries plain first, falls back to encrypted.
- Left/right Play and device type are in the advertisement's manufacturer data.
- Possibly a periodic keep-alive is needed; verify.
- References: github.com/ajchellew/zwiftplay (Play + encryption), github.com/TheInGoF/RideToWoosh (Ride on ESP32-S3, based on MAKINOLO's work), github.com/doudar/SHIFTR (virtual shifting).
- **Before writing ZAP code, capture real traffic** from the user's own controllers (nRF Connect logs; nRF52840 sniffer for encrypted devices). Build decoders against captured data, keep captures as test fixtures in `test/host/fixtures/`.
- Zwift firmware updates may change this. Keep a raw-dump debug mode and per-version decoding.

### Sensors

Heart rate 0x180D/0x2A37, external power 0x1818/0x2A63, cadence 0x1816/0x2A5B, BLE HID 0x1812 (later).

## Virtual shifting

Rider leaves the real chain in one gear (default 34×17, ratio 2.0). 24-gear virtual table, ratio 0.75–5.5 (`/config/gears.json`).
- v1, strategy A: SIM mode, `grade_cmd ≈ (grade + Crr) × k − Crr` with `k = virt_ratio / real_ratio`; small-angle, ignores aero, tune by feel; clamp −10…+20 %, max 2 writes/s.
- v2, strategy B: virtual speed from cadence × ratio × 2.105 m, road power `P = v·m·g·(sinθ + Crr·cosθ) + ½ρ·CdA·v³`, sent as ERG target at 2–4 Hz.
- Defaults: rider + bike 80 + 9 kg, CdA 0.32, Crr 0.004.

## Storage (microSD, FAT32)

| Path | Content |
|---|---|
| `/workouts/*.zwo` | Zwift workouts (Warmup, Ramp, SteadyState, IntervalsT, FreeRide, Cooldown, Cadence attr) |
| `/workouts/*.route` | routes from GPX (web app): `JustRide route 1`, `name=`, `step_m=10`, one elevation in cm per line; ≤ 20000 points (200 km), ≤ 256 KiB. Ridden in SIM by distance (`ride_start_route`), grade over 100 m, clamped like any SIM grade; flat after the end |
| `/rides/<user>/<YYYY-MM-DD_HHMM>.fit` (local start time, `tz`; never reuses an existing name) | file_id, device_info, event, record 1 Hz, lap, session, activity |
| `/rides/<user>/<id>.csv` | `t_s, ts_utc, power_w, cadence_rpm, speed_kmh, hr_bpm, target_w, grade_pct, gear, interval_idx, mode, lap` |
| `/rides/<user>/summary.csv` | append-only: `ride_id, start_utc, duration_s, moving_s, workout, ftp_w, avg_w, np_w, if, tss, kj, avg_hr, max_hr, avg_cad, distance_km, fw, weight_kg, user_max_hr` |
| `/rides/<user>/bests.csv` | append-only: `ride_id, duration_s, watts` for 1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600 s |
| `/rides/.journal` | crash recovery (ride id, start, user id, FTP, weight, max HR, UTC offset, workout); rebuild FIT/CSV on next boot; summary/bests rows are skipped if the ride id is already there |
| `/config/settings.json`, `/config/gears.json` | settings incl. riders (`users[]`: id `u<n>` never reused, name, ftp_w, ftp_known (false = default guess, untested), max_erg_w, weight_kg, max_hr; `user` = active id; `tz` POSIX TZ rule), gear table |
| `/www/stats/` | web app, pre-gzipped |
| `/plans/<user>.json` | training plan written by the web app (setup, sessions, hashes of the plan workouts it wrote); the device only stores it |
| NVS | remembered BLE device per role (written only when it changes); per-user roles (HR strap) keyed `<user>.r<role>`. Wi-Fi credentials live in `settings.json`; Wi-Fi driver storage is RAM-only (no flash wear) |

Recorder: buffer in PSRAM, flush every 10 s, fsync, journal. Summary + best efforts computed on device at ride end (sliding window over the in-memory ride).

Riders: up to 8, chosen on the home screen (only while idle). FTP is never changed automatically by a normal ride: if 95 % of a ride's best 20 min beats the rider's FTP, the summary screen offers "Set FTP to X W". A ramp test's result is applied at once (user's decision, 2026-10-30), the summary offers "Keep old FTP". FTP ramp test (built in, `workout_ramp_test()`; a `.zwo` with `JrRampTest="1"` on `<workout>` gets the same steps): steps from 1.5 W/kg +0.25 W/kg per minute (needs weight, not FTP), `ride.c` ends the ramp into the cool-down after 10 s under 60 rpm, under 80 % of target or without trainer data; result = 75 % of the best minute, applied even if lower (needs ≥ 2 min of ramp; also when the SD write failed). Untested riders (`ftp_known` false) get a home-screen button, a Today card and a test first in any new plan. Weight is recorded for W/kg only (FTMS SIM has no mass field).

## Web app (www/stats)

- Static web app served from SD by the device: `http://trainer.local/` redirects to `/stats/` (built-in fallback page when the app is missing). Dark-first design, system fonts, no external requests.
- Stats views parse the device CSVs in JS (fast start); DuckDB-WASM is only imported for the SQL console (column types given explicitly).
- Workouts: block model ↔ `.zwo` (only elements `zwo.c` parses; cool-down keeps Zwift's PowerLow = start). Generator types recovery … ramp test; "adapt to me" = form (TSB) factor 0.94–1.02, VO2max/sprint targets from 90-day 5-min/1-min bests. Generated files are checked with the device parser (`zwo_parse`).
- Plans: built in the browser; the next 7 days are written as `Plan <date> [<rider> - ]<type>.zwo` (device lists `Plan ` files first), regenerated on each visit unless the rider edited them (hash in the plan JSON). Missed sessions (`reschedule()` in plan.js, no ride that local day): a key session moves once to the next easy, non-long day within 7 days (that easy one is dropped, newest missed first); easy ones and repeat misses are dropped (`dropped`, `orig` in the plan JSON).
- Test locally with `tools/web_dev.py` (sample SD folder in `build/web_dev`, or `--device`).
- DuckDB-WASM 1.32.0 (EH bundle + worker, bundled with apache-arrow by esbuild) and uPlot 1.6.32 are served from the SD card under versioned `vendor/` paths, never from a CDN: the app must work with no internet. `tools/vendor_stats.sh` produces them.
- Pre-gzipped files (`*.gz`, served with `Content-Encoding: gzip`), `vendor/` cached `immutable`, everything else `no-cache` + ETag.
- **No OPFS:** it needs a secure context and `http://trainer.local` isn't one. `summary.csv` + `bests.csv` load on each visit; a ride's CSV loads when opened. DuckDB-WASM has no ICU: don't use `now() - INTERVAL` in SQL, pass UTC `TIMESTAMP` literals (`ago()` in app.js).
- Rider picker (default: active rider). Views: Today (form/fitness/fatigue, week, next plan session, recent rides), Rides (+ detail: power/HR/cadence charts with one y axis each, zones, intervals, HR drift), Power (curve, bests, FTP estimate), Workouts (builder, generator, "Add to calendar": day + preferred time, remembered in localStorage), Plan, Riders (add/rename/limits/active/delete), Device (files, uploads, OTA, SQL).
- Endpoints: `GET /` (redirect or fallback page), `GET /stats/*`, `GET /api/users` (id, name, active, ftp_w, ftp_known, max_erg_w, weight_kg, max_hr), `POST /api/users` (JSON `action` add/edit/select/delete; goes through the control task's `app_*` functions via `web_set_users()`, same limits, refused during a ride; answers with the new list), `POST /api/time` (browser clock as UTC seconds; only used while NTP hasn't synced and the clock is unset), `GET /api/rides`, `GET /rides/<user>/<file>`, `GET /api/workouts` (name, size, mtime), `GET /api/workout.ics?file=&start=<unix>&user=` (text/calendar, UID = start + file so re-adding updates), `GET|POST|DELETE /workouts/<file>.zwo|.route` (raw body, ≤ 64 KiB / 256 KiB; `/api/workouts` lists both), `GET|POST /plans/<user>.json` (≤ 32 KiB), `POST /ota` (raw .bin; one upload per tap of "Allow firmware update" on the device, within 5 min), `POST /www/stats/<path>` (web app files, ≤ 16 MiB, only inside that same window; `diag wifi allow` opens it from USB). Every request needs `Host` = `trainer.local` or an IPv4 address (DNS rebinding); every POST/DELETE needs an `X-JustRide` header (CSRF: forces a CORS preflight the device never grants).

## Testing

- Host unit tests (Unity or plain CMake + ctest) for every pure component: FTMS parsing (all flag combinations), control point encoding, ZAP decoding from captured fixtures, ZWO parsing (sample files in `tools/`), metrics (NP/IF/TSS against known values), vshift math, CSV and FIT output (validate FIT with the FIT SDK's FitCSVTool or fitdecode).
- On-device: a `diag` console command set (I2C scan, panel test pattern, SD speed test, BLE scan list, raw notify dump per device).
- UI changes: run `tools/ui_shots/shots.sh` and look at `docs/img/*.png` (renders the real `main/ui.c` with host LVGL; catches clipped or wrapping labels). Update the manual if a screen changed.
- Hardware-dependent behaviour is verified by the user on the real trainer and controllers; ask for logs rather than assuming.

## Operational safety (review every change against this)

Hardware must not be damaged by this code, and the rider must never be stuck with a dangerous load.

- Never write a chip register unless register and value are confirmed from a datasheet or Waveshare's own code. CH32V003: only the registers listed above; SYS_EN stays high.
- CH32: write the output register before the direction register. Outputs: EXIO1, 3, 4 (SD CS/D3, high), 5 (SYS_EN, high), 6; EXIO2 (touch INT) only low during the GT911 reset, then input; EXIO0 (ADC) and EXIO7 (RTC_INT) stay inputs.
- GPIO 1/2 are never driven by two peripherals at once: ST7701 init IO is deleted before the SDMMC host starts; CS 42 is held high. Don't touch GPIO 0, 19/20, 43/44.
- Buzzer only via `bsp_beep()` (timer-bounded, ≤ 1 s). No PWM on unknown expander pins.
- Flash wear: no periodic NVS/flash writes. NVS only on pairing/forget; Wi-Fi storage RAM-only; logs never go to flash. SD writes batched (10 s).
- Adaptive parts (`goal_t`, `Jr*` attributes on `SteadyState`, controller `adapt_update` in `ride.c`, see HANDOVER): the target never leaves the part's bounds or Max ERG; without HR it never rises. Hold-average: seconds without pedalling (cadence 0) are no shortfall, and the target rises at most 5 W per 10 s, so a rider who stops never comes back to a jump.
- Trainer: idle/pause/stop/finish → lowest resistance (flat SIM road if resistance mode is refused). ERG ≤ `max_erg_w` and ≤ 1500 W hard cap; SIM grade −10…+20 %; one control-point write in flight, ≤ 4/s, SIM ≤ 2/s. Spin-down is not exposed.
- The re-broadcast peripheral has no writable characteristic and no control point: nothing reaches the trainer through it. No bonding keys in flash (`CONFIG_BT_NIMBLE_NVS_PERSIST=n`).
- Auto-pause (`ride.c`, `AUTOPAUSE_*`): only on fresh trainer data; resumes by pedalling only if it paused itself (a tapped pause needs a tap); none in a ramp test's ramp. Paused = lowest resistance.
- Stopping a ride needs a long press. Wi-Fi is off while riding: only the control task switches it on, and it turns it off before a ride starts, outside the app lock (`web_stop()` waits for running web requests, which may need that lock).
- LAN trust (user's decision, 2026-10-30): the web app may edit riders, including Max ERG (≤ 1500 W), without on-device approval. Only firmware and web-app uploads need the device tap. Anything the web app reads from the SD card (plans, workouts) is untrusted input: validate it (`cleanPlan()` in plan.js). OTA needs on-device approval, checks the project name, and relies on bootloader rollback (`esp_ota_mark_app_valid_cancel_rollback()` only after full start-up).
- Stale sensor data (> 3 s trainer, > 5 s HR) is recorded as unknown, never repeated.
- Controller input: stop, skip and start need a held button; a disconnect never generates releases (a held button can't become a long press).
- Limits live in pure code (`ride.c`, `vshift.c`, `ftms_ctl.c`) and have host tests; keep it that way.

## Working with the user

- The user writes in English, prefers little or no soldering and is new to this board. Explain hardware steps plainly.
- The user has the KICKR Core and a **Zwift Ride** (plain ZAP). Buttons are verified against the user's capture (see ZAP above); paddles and long-press behaviour still to be tried on the bike.
- Don't claim protocol details are confirmed until they're checked against the user's own devices.

## Project memory

Claude maintains durable project notes in `.claude/memory/` (one fact per file, indexed below). Add, update or delete entries there instead of the user-level memory directory.

@.claude/memory/MEMORY.md
