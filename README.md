# JustRide

Standalone ESP32-S3 head unit for a Wahoo KICKR Core: free rides with virtual gears, ERG workouts from `.zwo` files, FIT/CSV recording on microSD and a browser statistics app served by the device. No cloud.

- **Manual (setup and operation):** [docs/manual.html](docs/manual.html) (open in a browser; screenshots in `docs/img/`)
- Project overview and rules: [CLAUDE.md](CLAUDE.md) · status: [HANDOVER.md](HANDOVER.md) · plan: `docs/plan.html`

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
