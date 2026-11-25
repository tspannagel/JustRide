---
name: bluetooth-working-config
description: BLE with KICKR + Zwift Ride + HR strap + Apple Watch works since ddeb320 (controller activities 10); don't touch the connect timing again without a log
metadata:
  type: project
---

The user confirmed on 2026-11-25 that Bluetooth works: trainer, Zwift Ride, HR strap and the watch re-broadcast all connect, after `CONFIG_BT_CTRL_BLE_MAX_ACT=10` (commit ddeb320). The earlier attempt to change connect timing ("listen 30 of 100 ms, back off, slower HR link", 272db54) was reverted (bb18bdd): the real cause was the controller's activity limit (HCI 0x07), not timing.

**Why:** Several rounds went into BLE connect problems; the fix was a controller config value, not code.
**How to apply:** Treat the current BLE connect/scan code and `BT_CTRL_BLE_MAX_ACT=10` as the known-good baseline. Any change to ble_mgr scan/connect parameters, link counts or controller config needs a reason from a device log and a re-test with all four links (3 sensors + watch). Internal RAM is tight: each extra activity costs ~500 B. [[operational-safety]]
