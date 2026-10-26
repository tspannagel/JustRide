# JustRide manual

JustRide turns a Waveshare ESP32-S3-Touch-LCD-4 into a head unit for a Wahoo KICKR Core: free rides with virtual gears, structured workouts in ERG mode, ride files on a microSD card, and a statistics page in your browser. No cloud, no account.

> **Status:** the firmware has not run on real hardware yet. Expect to fix things during the first bring-up (see [Troubleshooting](#9-troubleshooting)) and send logs back.

Contents
1. [What you need](#1-what-you-need)
2. [Safety](#2-safety)
3. [One-time setup](#3-one-time-setup)
4. [Riding](#4-riding)
5. [Workouts](#5-workouts)
6. [Wi-Fi, statistics and ride files](#6-wi-fi-statistics-and-ride-files)
7. [Firmware updates](#7-firmware-updates)
8. [Settings reference](#8-settings-reference)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. What you need

| Item | Notes |
|---|---|
| Waveshare ESP32-S3-Touch-LCD-4 (V3) | Check the version printed on the board. V4 may need pin changes. |
| microSD card, 32 GB or smaller | Formatted **FAT32**. Any speed class is fine. |
| Plain 5 V USB-A power supply + USB-A to USB-C cable | An old phone charger (5 V, 1–2 A) is ideal. See [Safety](#2-safety). |
| Wahoo KICKR Core | Bluetooth FTMS is used. Update its firmware with the Wahoo app once. |
| Heart-rate strap (optional) | Any Bluetooth strap (standard heart-rate service). |
| A Mac or PC | For building/flashing the firmware and preparing the SD card. |

No soldering, no extra wires.

## 2. Safety

**Power the board only from a plain 5 V USB supply or your computer's USB port.** Never use a laptop or phone *fast charger* (USB-C PD, up to 20 V): Waveshare reports that these burn protection parts on this board.

Built-in limits you can rely on while riding:

- ERG targets are capped at your *Max ERG* setting (default 600 W) and never exceed 1500 W, whatever a workout file says.
- When you **pause**, **stop**, or are on the home screen, the trainer is set to its **lowest resistance**.
- **Stop needs a long press**, so a stray tap mid-interval doesn't end the ride.
- Simulated gradients are limited to −10 … +20 %.
- Wi-Fi is switched **off** as soon as a ride starts.
- The firmware never runs a trainer spin-down and never writes to the board's battery/power chip.

Before a hard session, make sure the trainer and bike are set up as usual: the head unit only sends targets, it can't make the trainer itself safer.

## 3. One-time setup

### 3.1 Install the tools (macOS)

```sh
brew install cmake ninja dfu-util python3 node
mkdir -p ~/esp && cd ~/esp
git clone -b v5.3.2 --recursive https://github.com/espressif/esp-idf.git
./esp-idf/install.sh esp32s3
```

In **every new terminal** where you build, first run:

```sh
. ~/esp/esp-idf/export.sh
```

(Windows/Linux: follow Espressif's "ESP-IDF Get Started" guide for v5.3, then continue below.)

### 3.2 Build and flash

Connect the board to your computer with the USB-C cable, then in the project folder:

```sh
idf.py set-target esp32s3     # first time only
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

- The first build downloads LVGL, the display/touch drivers and mDNS (needs internet once).
- If flashing fails with "no serial port": hold the **BOOT** button, plug the cable in, release BOOT, and flash again.
- `monitor` shows the log. Leave it with `Ctrl+]`.

### 3.3 Prepare the SD card

On your computer, with the card inserted (here it's mounted as `/Volumes/JUSTRIDE`):

```sh
tools/vendor_stats.sh                     # once: downloads the statistics libraries (needs Node + internet)
tools/sd_deploy.sh /Volumes/JUSTRIDE      # copies the statistics app and sample workouts
```

The card then contains:

```
/www/stats/     statistics app
/workouts/      .zwo workout files (two samples)
/rides/         your rides (filled by the device)
/config/        settings.json, gears.json (created on first boot)
```

Eject the card and put it in the board's microSD slot (contacts facing the board). Insert or remove the card only while the board is **unplugged**.

### 3.4 First start

Power the board. The screen shows the home screen: **Free ride**, **Workouts**, **Settings**, **Wi-Fi**, and a status line.

On first boot the device writes `/config/settings.json` with defaults and a random password for its own Wi-Fi hotspot. To enter your FTP and home Wi-Fi, either:

- use **Settings** on the device for FTP and Max ERG, and
- edit `settings.json` on your computer for Wi-Fi (unplug the board, move the card, edit, move it back):

```json
{
  "ftp_w": 230,
  "max_erg_w": 600,
  "crr": 0.004,
  "cw": 0.51,
  "wifi_ssid": "MyHomeWifi",
  "wifi_password": "secret",
  "ap_password": "k7m2xq9fha"
}
```

`settings.json` is a plain text file, so anyone with the card can read the Wi-Fi password in it.

### 3.5 Connect the trainer, heart-rate strap and Zwift Ride

1. **Close Zwift, the Wahoo app and any other app** that might connect to the KICKR or the Zwift Ride.
2. Pedal a few strokes to wake the KICKR.
3. The status line changes from *Trainer: searching…* to *Trainer: connected* (usually within a few seconds).
4. Put on the heart-rate strap (moisten the contacts); it connects the same way.
5. Wake the Zwift Ride by pressing a button. The home screen shows *Zwift Ride: connected*.

The device remembers the first trainer, strap and Zwift controller it finds and from then on connects only to those. To pair different ones: **Settings → Forget paired devices**.

### 3.6 Set the clock

Ride files are named after the start time. The board has a battery-backed clock, but it has to be set once: turn on **Wi-Fi** with your home network configured (see [6](#6-wi-fi-statistics-and-ride-files)). The device fetches the time from the internet and stores it. Until then the home screen shows *Clock not set*. Rides still record, but with wrong dates.

## 4. Riding

### Free ride

Tap **Free ride**. The trainer simulates a road; you change **virtual gears** with **−** and **+** and the road's gradient with **Grade −** / **Grade +** (0.5 % steps).

Leave the bike's real chain in one gear, by default **34 × 17** (front × rear). Virtual gears make the trainer harder or easier as if you shifted. If your real gear is different, set `real_ratio` in `gears.json` (see [8](#8-settings-reference)).

### Ride screen

```
 0:23:41                         Gear 12/24        <- time, interval or gear
                 245 W                             <- 3-second power
      Target 250 W (100 %)  90 rpm                 <- ERG target / road gradient
   90 rpm      142 bpm      31.2 km/h
   Avg 231 W   NP 238 W   412 kJ   Lap 2
 [ − ]   [ Pause ]   [ Lap ]   [ + ]
 [Grade −] [Skip] [ Hold to stop ] [Grade +]
```

| Control | Free ride / free segment | ERG workout segment |
|---|---|---|
| **−** / **+** | virtual gear down / up | intensity −5 % / +5 % (50–150 %) |
| **Grade − / +** | road gradient | hidden |
| **Skip** | – | jump to the next interval |
| **Pause** | timer stops, lowest resistance | same |
| **Lap** | starts a new lap in the ride file | same |
| **Hold to stop** | press and hold to end the ride | same |

### Zwift Ride buttons

| Button | Action |
|---|---|
| Shift up / down (either side) | same as **+** / **−** on screen: virtual gear, or ERG intensity ±5 % |
| D-pad up / down | road gradient +/− 0.5 % (free ride) |
| **A** (short) | pause / resume; on the summary screen: done |
| **A** (hold 1 s) on the home screen | start a free ride |
| **Y** (short) | lap |
| D-pad right (hold 1 s) | skip to the next interval |
| **B** (hold 1 s) | stop the ride |

Menus (workout list, settings, Wi-Fi) are touch-only.

The button layout is based on other people's reverse-engineering and is **not yet verified on a real Zwift Ride**. If a button does the wrong thing or nothing, see [Troubleshooting](#9-troubleshooting).

Beeps: **1** = next interval in 3 seconds, **2** = new interval started, **3** = workout finished (you continue in free ride until you stop).

### After the ride

The summary shows time, average power, NP, TSS, IF, kJ, distance and average heart rate. Tap **Done**. The ride is saved as `.csv` and `.fit` in `/rides/`. Rides shorter than 60 seconds are discarded.

If the power fails mid-ride, the ride is not lost: everything up to the last 10 seconds is on the card, and the FIT file and summary are completed on the next start.

## 5. Workouts

Workouts are Zwift `.zwo` files in `/workouts/` on the SD card. Supported: `Warmup`, `Cooldown`, `Ramp`, `SteadyState`, `IntervalsT`, `FreeRide`, `MaxEffort` (as free ride), and `Cadence` targets. Power values are fractions of your FTP (`0.9` = 90 %).

Add workouts by copying `.zwo` files to the card, or over Wi-Fi from the device's start page (see below). Tap **Workouts**, then a file, to start it.

A minimal example:

```xml
<workout_file>
  <name>Threshold 2x20</name>
  <workout>
    <Warmup Duration="600" PowerLow="0.5" PowerHigh="0.75"/>
    <IntervalsT Repeat="2" OnDuration="1200" OffDuration="300" OnPower="0.95" OffPower="0.55" Cadence="90"/>
    <Cooldown Duration="300" PowerLow="0.6" PowerHigh="0.4"/>
  </workout>
</workout_file>
```

## 6. Wi-Fi, statistics and ride files

Wi-Fi is only for when you're **not** riding. On the home screen tap **Wi-Fi → Turn on**. The screen shows how to reach the device:

- **Home network** (if `wifi_ssid` is set): open `http://trainer.local/` (or the IP address shown) in a browser on the same network.
- **Own hotspot** (if no home network is set, or it isn't reachable within 15 seconds): connect your phone/laptop to the Wi-Fi **JustRide** with the password shown on the screen, then open `http://192.168.4.1/`.

The start page lists your ride files (click to download; the `.fit` files can be uploaded to intervals.icu, Strava, Garmin Connect, …), lets you upload `.zwo` workouts, and links to the **statistics app**:

- **Overview:** rides and hours, fitness (CTL), fatigue (ATL) and form (TSB) over time.
- **Power curve:** your best power for 1 s … 60 min, all-time vs. last 90 days, and an FTP estimate (95 % of best 20 min).
- **Rides:** every ride; click one for the power/HR chart, time in power zones, HR drift and how well you hit each interval.
- **SQL:** query `summary`, `bests` and `ride` (the last opened ride) yourself.

The statistics app runs entirely in your browser and needs no internet connection.

Wi-Fi turns off automatically when you start a ride.

## 7. Firmware updates

**Over USB:** `idf.py -p /dev/cu.usbmodem* flash` as in 3.2.

**Over Wi-Fi:**
1. Build on your computer: `idf.py build` → the file is `build/justride.bin`.
2. On the device: **Wi-Fi → Turn on → Allow firmware update**. Updates are accepted for 5 minutes after this tap; without it the device refuses them, so nobody else on your network can flash it.
3. On the device's start page choose the `.bin` under *Firmware update*. Don't unplug the board while it says "flashing".
4. The device restarts. If the new firmware fails to start, the device automatically goes back to the previous one.

## 8. Settings reference

### `/config/settings.json`

| Key | Default | Range | Meaning |
|---|---|---|---|
| `ftp_w` | 200 | 50–600 | Your FTP; workouts are scaled to it |
| `max_erg_w` | 600 | 100–1500 | Highest ERG target the device will ever send |
| `crr` | 0.004 | 0.001–0.02 | Rolling resistance sent to the trainer in free ride |
| `cw` | 0.51 | 0.1–2.0 | Wind resistance coefficient (kg/m) in free ride |
| `wifi_ssid`, `wifi_password` | empty | | Your home Wi-Fi |
| `ap_password` | random | ≥ 8 chars | Password of the device's own hotspot |

Invalid or out-of-range values are ignored and the default is used. If the file is not valid JSON (a missing quote or comma), the device uses defaults for everything, logs `settings.json is not valid JSON` and leaves your file unchanged so you can fix it.

### `/config/gears.json` (optional)

Without this file there are 24 virtual gears from ratio 0.75 to 5.5, and the real gear is assumed to be 34 × 17 (ratio 2.0).

```json
{ "real_ratio": 2.0, "ratios": [0.75, 0.9, 1.05, 1.2, 1.4, 1.6, 1.8, 2.0, 2.25, 2.5, 2.8, 3.1, 3.5, 4.0, 4.6, 5.5] }
```

`real_ratio` = front teeth ÷ rear teeth of the gear your chain is actually in. `ratios`: 2–30 values, ascending.

## 9. Troubleshooting

Keep the USB cable connected to your computer and run `idf.py -p /dev/cu.usbmodem* monitor`: the log explains most problems. At the `jr>` prompt these commands help:

| Command | What it does |
|---|---|
| `help` | list commands |
| `i2c` | list chips on the internal bus (expect 0x20 or 0x38, 0x51, 0x5D or 0x14) |
| `pattern 0` / `1` / `2` | colour bars / white / black on the screen |
| `sd` | SD card write/read speed test |
| `buzz` | short beep |
| `mem` | free memory |
| `zap dump on` / `off` | log every packet from the Zwift Ride as hex |

| Problem | What to try |
|---|---|
| Screen stays dark | Check the log for `TCA9554 not found` or I2C errors and send it. Run `i2c`. |
| Colours wrong, image shifted or flickering | Run `pattern 0` (should be white, yellow, cyan, green, magenta, red, blue, black from left to right). Send a photo; the display settings in `bsp.c` need adjusting. |
| Touch doesn't react | Log says `GT911 not found`? Send the `i2c` output. |
| *No SD card* on the home screen | Card must be FAT32, 32 GB or smaller, inserted with the board unplugged. Run `sd`. |
| Trainer stays *searching* | Close every other app connected to the KICKR, pedal to wake it, then try **Settings → Forget paired devices**. |
| Zwift Ride doesn't connect | Close the Zwift app (on phone, tablet and PC), press a button to wake it, check the log for `zap`. |
| Zwift Ride button does the wrong thing / nothing | Run `zap dump on`, press each button once slowly (note the order), and send the log. The button table is then corrected from your recording. |
| Trainer connects but resistance doesn't change | Check the log for `request control refused` or `rejected`; another app may still have control. |
| Board switches itself off | A known issue of this board's power chip at low load; the fix isn't implemented yet. Use the computer's USB port for now and report it. |
| *Clock not set* | Turn on Wi-Fi with your home network configured (needs internet). |
| `trainer.local` doesn't open | Use the IP address shown on the device instead (some Android phones don't support `.local` names). |
| Firmware update refused | Tap **Allow firmware update** on the device first (valid 5 minutes). |

Not supported yet: Zwift Click and Play (encrypted protocol), menu navigation with the Zwift Ride, power meters and cadence sensors other than the trainer, intervals.icu upload.
