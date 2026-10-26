# KICKR Head Unit

Standalone ESP32-S3 head unit for indoor cycling. It controls a Wahoo KICKR Core over Bluetooth (FTMS), reads all Zwift controllers (Click v1/v2, Play, Ride), runs structured workouts from microSD, records rides locally (FIT + CSV) and shows live stats on a 4" touchscreen. No cloud, no 3D world. A browser-based statistics app (DuckDB-WASM) is served by the device itself over Wi-Fi.

Full plan with diagrams: `docs/plan.html` (open in a browser; Mermaid diagrams may not render offline). `HANDOVER.md` has current status, decisions and open questions.

## Hardware

**Board: Waveshare ESP32-S3-Touch-LCD-4, V3.** Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4 (schematic PDFs for V1–V4 and an official STEP model are linked under Resources).

- ESP32-S3-N16R8: 16 MB flash, 8 MB octal PSRAM, BLE 5, Wi-Fi 2.4 GHz
- 4" IPS 480×480, ST7701, 16-bit RGB565 parallel bus (frame buffers in PSRAM)
- GT911 capacitive touch, PCF85063 RTC, TCA9554 I/O expander, SW6106 battery/power chip
- microSD over SPI, onboard buzzer, USB-C (native USB: flashing + console)
- RS485, CAN, 7–36 V terminal: present, unused
- **No free GPIOs.** Do not plan features that need extra wires.

### Pin map (from Waveshare wiki, verify against V3 schematic)

| Function | GPIO |
|---|---|
| LCD R1–R5 | 46, 3, 8, 18, 17 |
| LCD G0–G5 | 14, 13, 12, 11, 10, 9 |
| LCD B1–B5 | 5, 45, 48, 47, 21 |
| LCD HSYNC / VSYNC / DE / PCLK | 38 / 39 / 40 / 41 (Waveshare benchmark: PCLK 21 MHz) |
| ST7701 init 3-wire SPI SDA / SCL / CS | 1 / 2 / 42 |
| SD MOSI / SCK / MISO | 1 / 2 / 4 (shares 1 and 2 with LCD init), CS = EXIO3 |
| I2C SDA / SCL | 15 / 7 (touch, RTC, expander, SW6106) |
| Touch INT | 16 |
| USB D− / D+ | 19 / 20 |
| RS485 RX / TX (unused) | 43 / 44 |
| CAN RX / TX (unused) | 0 / 6 (GPIO0 is also BOOT) |

TCA9554 outputs: EXIO0 touch reset, EXIO1 backlight enable, EXIO2 LCD reset, EXIO3 SD CS, EXIO4 "BLC" (unknown, check schematic), EXIO5 buzzer enable, EXIO6 RTC interrupt, EXIO7 "DO1".

**Known wiki inconsistency:** one section says the expander is on GPIO 8/9, but 8/9 are LCD data lines. Assume everything I2C is on 15/7 and confirm with an I2C scan at first boot. Don't trust this table blindly; read the V3 schematic PDF when something doesn't respond.

### Bring-up order (shared pins make this mandatory)

1. I2C on 15/7, scan, probe TCA9554, GT911, PCF85063, SW6106
2. SW6106: apply the light-load register fix from the Waveshare FAQ (the chip otherwise switches the board off at low load)
3. TCA9554: backlight off, pulse LCD reset (EXIO2) and touch reset (EXIO0)
4. ST7701 init over 3-wire SPI (1/2/42), then hold CS 42 high permanently
5. Start the RGB panel (`esp_lcd_panel_rgb`), two 480×480 RGB565 frame buffers in PSRAM
6. Re-configure 1/2/4 as the SD SPI bus, CS via EXIO3, mount FATFS
7. Backlight on, start LVGL, then BLE, then the control loop

RGB + PSRAM: PSRAM 80 MHz octal, bounce buffers on, `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA` on. Wi-Fi traffic can make the RGB image drift; Wi-Fi is only on when not riding.

### Power safety

Use a plain 5 V USB-A supply and a USB-A to USB-C cable. Waveshare reports fast-charge sources (up to 20 V) burning protection parts on this board. Never suggest powering it from a laptop/phone fast charger. During development, the computer's USB port is fine.

## Firmware stack

- **ESP-IDF ≥ 5.3**, C (C11). Arduino is not used (confirmed by the user).
- NimBLE host as BLE central, up to 6 simultaneous links
- LVGL 9 via `esp_lvgl_port`, direct mode on full frame buffers
- mbedTLS (ECDH P-256, HKDF-SHA256, AES-CCM) for encrypted Zwift controllers
- nanopb for Zwift protobuf messages (when ZAP is implemented)
- Own minimal FIT encoder (`recorder/fit.c`, ~300 lines) instead of the Garmin FIT SDK; validated with fitdecode
- FreeRTOS: core 0 = NimBLE host + Wi-Fi, core 1 = control loop, LVGL, recorder, storage

### Repo layout

```
main/                     app_main (bring-up order), app.c control task, ui.c LVGL screens, diag.c console
components/
  bsp_s3lcd4/             board support: I2C, TCA9554, ST7701 init, RGB panel, SD, RTC, touch, buzzer
  evbus/                  jr_types.h (stable event/sample types, pure) + one FreeRTOS queue
  ble_mgr/                NimBLE central: scan, connect, GATT discovery, CCCD subscribe, back-off, role per device (NVS)
  drv_ftms/               FTMS trainer driver: metrics out, control point with one write in flight
  drv_hr/                 heart-rate strap (0x180D)
  drv_zap/                Zwift Ride (plain ZAP): handshake, button edges -> input events, raw hex dump
  proto_ftms/             pure codec: Indoor Bike Data parsing, control point encode/decode
  proto_zap/              pure codec: RideOn handshake, 0x23 keypad protobuf, button table (unverified)
  input_map/              pure: input events -> ride actions by ride state (long press for stop/skip/start)
  workout/                pure: .zwo parser + workout timeline
  vshift/                 pure: gear table, strategy-A grade scaling
  metrics/                pure: NP, IF, TSS, kJ, distance, best efforts
  ride/                   pure: ride state machine, trainer command, safety limits
  recorder/               csv.c/fit.c pure writers; recorder.c SD files, journal, crash recovery
  settings/               settings.json, gears.json
  web/                    Wi-Fi (STA, AP fallback), HTTP, mDNS, SNTP, OTA
www/stats/                statistics app (vendor/ generated by tools/vendor_stats.sh, not committed)
test/host/                host unit tests (CMake + ctest, ASan/UBSan)
tools/                    sample .zwo files, vendor_stats.sh, sd_deploy.sh
docs/                     plan.html (planning), manual.md (user manual: setup + operation)
```

Not built yet: encrypted ZAP (Play, Click v1; the user owns a Zwift Ride only), menu navigation by controller, cycling-power fallback, intervals.icu upload.

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
- Trainer control point: one command in flight, wait for the 0x80 response (≤ 1 s timeout), max 4 writes/s, SIM grade updates max 2/s. After reconnect: Request Control, Start, resend current target.

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

### Zwift Accessory Protocol (ZAP) — community reverse-engineered, NOT verified

| Item | UUID |
|---|---|
| Service | 00000001-19CA-4651-86E5-FA29DCDD09D1 |
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
| `/rides/<YYYY-MM-DD_HHMM>.fit` | file_id, device_info, event, record 1 Hz, lap, session, activity |
| `/rides/<id>.csv` | `t_s, ts_utc, power_w, cadence_rpm, speed_kmh, hr_bpm, target_w, grade_pct, gear, interval_idx, mode, lap` |
| `/rides/summary.csv` | append-only: `ride_id, start_utc, duration_s, moving_s, workout, ftp_w, avg_w, np_w, if, tss, kj, avg_hr, max_hr, avg_cad, distance_km, fw` |
| `/rides/bests.csv` | append-only: `ride_id, duration_s, watts` for 1, 5, 10, 30, 60, 120, 300, 600, 1200, 1800, 3600 s |
| `/rides/.journal` | crash recovery; rebuild FIT/CSV on next boot |
| `/config/settings.json`, `/config/gears.json` | settings, gear table |
| `/www/stats/` | statistics app, pre-gzipped |
| NVS | remembered BLE device per role (written only when it changes). Wi-Fi credentials live in `settings.json`; Wi-Fi driver storage is RAM-only (no flash wear) |

Recorder: buffer in PSRAM, flush every 10 s, fsync, journal. Summary + best efforts computed on device at ride end (sliding window over the in-memory ride).

## Statistics app (www/stats)

- Static web app served from SD by the device: `http://trainer.local/stats/` (mDNS).
- DuckDB-WASM 1.32.0 (EH bundle + worker, bundled with apache-arrow by esbuild) and uPlot 1.6.32 are served from the SD card under versioned `vendor/` paths, never from a CDN: the app must work with no internet. `tools/vendor_stats.sh` produces them.
- Pre-gzipped files (`*.gz`, served with `Content-Encoding: gzip`), `vendor/` cached `immutable`, everything else `no-cache` + ETag.
- **No OPFS:** it needs a secure context and `http://trainer.local` isn't one. `summary.csv` + `bests.csv` load on each visit; a ride's CSV loads when opened. DuckDB-WASM has no ICU: don't use `now() - INTERVAL` in SQL, pass UTC `TIMESTAMP` literals (`ago()` in app.js).
- Views: overview (CTL 42 d / ATL 7 d / TSB from daily TSS, computed in JS), power curve (all-time vs 90 d, FTP = 95 % of best 20 min), ride detail (time series, zones, interval compliance, HR drift), SQL console.
- Endpoints: `GET /` (start page), `GET /stats/*`, `GET /api/rides`, `GET /rides/<file>`, `POST /workouts/<file>.zwo` (raw body, ≤ 64 KiB), `POST /ota` (raw .bin; one upload per tap of "Allow firmware update" on the device, within 5 min). Every request needs `Host` = `trainer.local` or an IPv4 address (DNS rebinding); every POST needs an `X-JustRide` header (CSRF: forces a CORS preflight the device never grants).

## Testing

- Host unit tests (Unity or plain CMake + ctest) for every pure component: FTMS parsing (all flag combinations), control point encoding, ZAP decoding from captured fixtures, ZWO parsing (sample files in `tools/`), metrics (NP/IF/TSS against known values), vshift math, CSV and FIT output (validate FIT with the FIT SDK's FitCSVTool or fitdecode).
- On-device: a `diag` console command set (I2C scan, panel test pattern, SD speed test, BLE scan list, raw notify dump per device).
- Hardware-dependent behaviour is verified by the user on the real trainer and controllers; ask for logs rather than assuming.

## Operational safety (review every change against this)

Hardware must not be damaged by this code, and the rider must never be stuck with a dangerous load.

- Never write SW6106 (power/battery chip) registers unless the exact register and value are confirmed from Waveshare docs. Same for any chip register not in a datasheet.
- TCA9554: write the output register before the direction register; unknown pins (EXIO4, 6, 7) stay inputs.
- GPIO 1/2 are never driven by two peripherals at once: ST7701 init IO is deleted before the SD SPI bus starts; CS 42 is held high. Don't touch GPIO 0, 19/20, 43/44.
- Buzzer only via `bsp_beep()` (timer-bounded, ≤ 1 s). No PWM on unknown expander pins.
- Flash wear: no periodic NVS/flash writes. NVS only on pairing/forget; Wi-Fi storage RAM-only; logs never go to flash. SD writes batched (10 s).
- Trainer: idle/pause/stop/finish → lowest resistance (flat SIM road if resistance mode is refused). ERG ≤ `max_erg_w` and ≤ 1500 W hard cap; SIM grade −10…+20 %; one control-point write in flight, ≤ 4/s, SIM ≤ 2/s. Spin-down is not exposed.
- Stopping a ride needs a long press. Wi-Fi is off while riding. OTA needs on-device approval, checks the project name, and relies on bootloader rollback (`esp_ota_mark_app_valid_cancel_rollback()` only after full start-up).
- Stale sensor data (> 3 s trainer, > 5 s HR) is recorded as unknown, never repeated.
- Controller input: stop, skip and start need a held button; a disconnect never generates releases (a held button can't become a long press).
- Limits live in pure code (`ride.c`, `vshift.c`) and have host tests; keep it that way.

## Working with the user

- The user writes in English, prefers little or no soldering and is new to this board. Explain hardware steps plainly.
- The user has the KICKR Core and a **Zwift Ride** (plain ZAP). The Ride code was written before any capture: verify button bits/polarity with `zap dump on` and turn the dumps into fixtures in `test/host/fixtures/`.
- Don't claim protocol details are confirmed until they're checked against the user's own devices.

## Project memory

Claude maintains durable project notes in `.claude/memory/` (one fact per file, indexed below). Add, update or delete entries there instead of the user-level memory directory.

@.claude/memory/MEMORY.md
