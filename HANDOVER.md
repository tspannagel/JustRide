# Handover: KICKR Head Unit

Planning was done in a Claude chat on 26 Oct 2026. M0 firmware scaffolding started the same day. Read `CLAUDE.md` first; `docs/plan.html` has the full plan with diagrams.

## Status

- Planning: done (hardware choice, protocols, architecture, data formats, stats app, milestones).
- Hardware: Waveshare ESP32-S3-Touch-LCD-4 V3 chosen. Not confirmed whether the user has received it yet.
- Firmware: M0 scaffold written but **not yet built** (no ESP-IDF on the dev machine yet). `bsp_s3lcd4` (bring-up steps 1, 3–6), `diag` console, `proto_ftms` codec with passing host tests.

## Decisions made

| Topic | Decision |
|---|---|
| Framework | ESP-IDF ≥ 5.3, C11 (confirmed 2026-10-26) |
| Board | Waveshare ESP32-S3-Touch-LCD-4 V3, no soldering, no custom PCB |
| Power | Plain 5 V USB-A supply + USB-A to C cable; no fast chargers |
| Trainer protocol | Bluetooth FTMS (not Cycling Power, not ANT+) |
| Controllers | All Zwift controllers: Ride, Click v2 (plain ZAP), Play, Click v1 (encrypted ZAP); Sterzo and BLE HID later |
| Navigation | Touch + Zwift controllers; no hardware buttons |
| Workouts | `.zwo` files on microSD, ERG with FTP scaling |
| Virtual shifting | Strategy A (SIM grade scaling) first, strategy B (physics → ERG) later |
| Recording | FIT + per-second CSV + summary/bests CSV, all local |
| Statistics | DuckDB-WASM app served by the device from SD, vendored assets, OPFS cache |
| Wi-Fi | Only off the bike (files, stats, OTA); off while riding |
| Out of scope | 3D world, multiplayer, ANT+ |
| Case | User designs their own from Waveshare's official STEP model (wiki → Resources → "3D STP") |

## Open questions (ask the user)

1. Which Zwift controllers do they own? Decides M4 vs M5 order.
2. Has the board arrived, and which version is printed on it (V3 or V4)? V4 has its own demo repo: github.com/waveshareteam/ESP32-S3-Touch-LCD-4.
3. Record alone, or also re-broadcast as an FTMS peripheral so a watch records the same ride?

## Must verify on real hardware (don't assume)

- I2C bus pins for the TCA9554 (wiki contradicts itself: 15/7 vs 8/9). I2C scan at first boot.
- What TCA9554 EXIO4 ("BLC") and EXIO7 ("DO1") do; whether backlight dimming is possible.
- SW6106 light-load register fix (Waveshare FAQ). Not implemented: register unknown, TODO in `bsp.c`.
- ST7701 init sequence: `bsp.c` uses the `esp_lcd_st7701` default; swap in Waveshare's table if colours/gamma are off.
- RGB timings (porches, 16 MHz PCLK) in `bsp.c` are generic ST7701 values; raise PCLK towards 21 MHz once stable.
- SD with no SPI CS pin (EXIO3 held low) mounts reliably.
- ZAP UUIDs, handshake and message layouts on the user's own controllers (capture first).
- Whether Click v2 / Ride really run unencrypted.
- Whether a ZAP keep-alive is needed.
- KICKR Core firmware exposes FTMS (expected yes) and the Indoor Bike Data flags it uses.
- DuckDB-WASM OPFS persistence in the pinned version.

## Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | Bench setup | ESP-IDF project builds and flashes; I2C scan, panel test pattern, SD mount work; nRF Connect captures of KICKR and each controller saved to `tools/captures/` |
| M1 | FTMS read + display | Live power, cadence, speed on screen; auto-reconnect |
| M2 | ERG workouts | A `.zwo` from SD runs end to end with FTP scaling and beeps |
| M3 | Recording + file manager | FIT opens in intervals.icu / Golden Cheetah; CSVs match schema; Wi-Fi upload/download work |
| M3b | Statistics app | DuckDB-WASM page from SD shows overview, power curve, ride detail; second visit loads only new rides |
| M4 | Ride + Click v2 | All buttons produce InputEvts; menus navigable without touch |
| M5 | Play + Click v1 | Encrypted handshake reliable over 50 reconnects |
| M6 | Virtual shifting | Strategy A feels right across 24 gears; gear shown on screen |
| M7 | Case + mount | User's printed case, bar or stand mount |
| M8 | Extras | HR, Sterzo, HID, fan via smart plug, intervals.icu upload, OTA |

## Suggested first prompt for Claude Code

> Read CLAUDE.md and HANDOVER.md. Ask me the open questions first. Then start M0: scaffold the ESP-IDF project with the repo layout from CLAUDE.md, implement `bsp_s3lcd4` following the bring-up order (I2C scan with results printed, SW6106 fix, TCA9554, ST7701 init, RGB panel with a test pattern, SD mount with a read/write speed test, buzzer beep), plus a serial `diag` console. Set up host unit tests with one trivial test so the test pipeline exists. Tell me exactly how to build, flash and what output to send back.

## Suggested second prompt (after M0 works)

> Start M1: implement `evbus`, `ble_mgr` (scan, connect, reconnect, store trainer in NVS), `proto_ftms` with host tests for all Indoor Bike Data flag combinations, `drv_ftms`, and a minimal LVGL screen with power, cadence and speed. Use my nRF Connect capture in tools/captures/ as a test fixture.
