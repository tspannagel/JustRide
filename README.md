# JustRide

Standalone ESP32-S3 head unit for a Wahoo KICKR Core: free rides with virtual gears, ERG workouts from `.zwo` files, FIT/CSV recording on microSD and a browser statistics app served by the device. No cloud.

- **Manual (setup and operation):** [docs/manual.html](docs/manual.html) (open in a browser; screenshots in `docs/img/`)
- Project overview and rules: [CLAUDE.md](CLAUDE.md) · status: [HANDOVER.md](HANDOVER.md) · plan: `docs/plan.html`

## Deploy everything (3 steps)

From the repo on `main`, head unit on USB, idle (not riding) and on your Wi-Fi:

```sh
# 1. Firmware + web app + manual: build and flash over USB. On its first start the new firmware
#    copies its web app to the SD card (~10 s). OTA from the web app's Device page does the same.
. ~/esp/esp-idf/export.sh && idf.py build && idf.py -p /dev/cu.usbmodem* flash

# 2. DuckDB for the SQL console (34 MB, not inside the firmware): only on a new SD card or a new
#    DuckDB version; tap Wi-Fi -> Allow firmware update on the device when asked
[ -d www/stats/vendor ] || tools/vendor_stats.sh
tools/web_deploy.sh --check || tools/web_deploy.sh

# 3. NAS (optional): on the NAS `docker compose up -d --build` in nas/ (see nas/README.md),
#    or on this computer, then open http://localhost:8080/
python3 nas/justride_nas.py
```

`tools/web_deploy.sh` also sends a web app change without new firmware; it stays until the next firmware. First time only: `idf.py set-target esp32s3`, and the SD card set up as in the manual.

## Build (ESP-IDF ≥ 5.3)

```sh
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Power: plain 5 V USB supply or the computer's USB port, never a fast charger.

## SD card

```sh
tools/vendor_stats.sh                  # once: pinned DuckDB-WASM + uPlot into www/stats/vendor
tools/sd_deploy.sh /Volumes/<card>     # statistics app + sample workouts
```

## Releases

Push a SemVer tag and `.github/workflows/release.yml` builds and publishes a GitHub release:

```sh
git tag v1.0.0-alpha.1 && git push origin v1.0.0-alpha.1   # anything after "-" = pre-release
git tag v1.0.0 && git push origin v1.0.0
```

Assets: `justride-<version>.bin` (OTA through the web app), `justride-<version>-full.bin` (first USB flash at 0x0), `justride-www-<version>.zip` (web app for the SD card, gzipped, with vendor/) and `SHA256SUMS`. The tag without the `v` becomes the firmware version (`version.txt` → `esp_app_desc`), max 31 characters.

## Host tests

```sh
cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
```

## Layout

```
main/          app_main, control task, LVGL UI, diag console
components/    bsp_s3lcd4, evbus, ble_mgr, drv_ftms, drv_hr, proto_ftms,
               workout, vshift, metrics, ride, recorder, settings, web
www/stats/     statistics app
test/host/     host unit tests for the pure components
tools/         sample workouts, vendoring and SD scripts
```
