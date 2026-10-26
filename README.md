# JustRide

Standalone ESP32-S3 head unit for a Wahoo KICKR Core. See `CLAUDE.md` for the project overview and `HANDOVER.md` for status.

## Firmware (ESP-IDF ≥ 5.3)

One-time setup: install ESP-IDF following Espressif's "Get Started" guide for macOS, then in each new terminal run `. $IDF_PATH/export.sh` (or `get_idf`).

```sh
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Use a plain 5 V USB supply or the computer's USB port; never a fast charger.

On the USB console (`jr>` prompt): `help`, `i2c`, `pattern [0|1|2]`, `sd`, `buzz`, `mem`.

## Host tests

```sh
cmake -S test/host -B test/host/build && cmake --build test/host/build && ctest --test-dir test/host/build
```

## Layout

```
main/                  app_main + diag console
components/bsp_s3lcd4  board bring-up: I2C, TCA9554, ST7701/RGB panel, SD
components/proto_ftms  pure FTMS codec (host-tested)
test/host/             host unit tests
```
