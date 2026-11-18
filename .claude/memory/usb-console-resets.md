---
name: usb-console-resets
description: Opening the board's USB serial port resets the ESP32-S3, even with DTR/RTS held low
metadata:
  type: project
---

Opening `/dev/cu.usbmodem2101` (USB-Serial-JTAG) restarts the head unit (`rst:0x15 USB_UART_CHIP_RESET`), also from pyserial with `dtr=False, rts=False` set before `open()`. Seen 2026-11-18. To read the log or run `diag` commands, open the port once and keep it open for the whole test (IDF venv has pyserial: `~/.espressif/python_env/idf5.3_py3.15_env/bin/python`).

**Why:** Two unannounced reboots while checking Wi-Fi state.
**How to apply:** Never open the console while a ride may be running (check `curl -4 http://trainer.local/api/info` answers = Wi-Fi on = not riding). Tell the user beforehand that it reboots the device. [[operational-safety]]
