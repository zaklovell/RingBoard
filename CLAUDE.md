# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this
repository.

## What this is

**RingBoard**: Oura ring stats on a CYD board (ESP32-2432S028R, the "Cheap Yellow
Display"). Shows readiness + sleep scores, sleep stage breakdown, and activity
(steps, distance, calories, inactive time). Built and compiling; not yet flashed.
The hardware knowledge below was earned on an identical board (Zak owns two from
the same 2-pack) and is all verified.

The project was renamed from OuraCYD to RingBoard because the Oura API Agreement
(section 6(e)) forbids "Oura" in the app name. The registered Oura app, the GitHub
repo (github.com/zaklovell/RingBoard), and the Pages site all say RingBoard; only
this local folder still carries the old name.

## Project state and decisions

- `src/` firmware mirrors the planes project structure: config.h, models.h,
  oura.cpp (OAuth + filtered fetches), ui.cpp (three-band sprite UI, Oura dark
  theme), webapi.cpp (HA API at `ringboard.local`), main.cpp.
- `tools/oura_auth.py`: one-time browser OAuth dance; writes the refresh token
  into `src/secrets.h`. Redirect URI is `http://localhost:8080/callback`.
- Token lifecycle: Oura rotates refresh tokens, so the firmware persists the
  current one in NVS (`Preferences`, namespace "ringboard", key "rtok"); the
  compiled secrets.h value is only a first-boot seed.
- `OURA_USE_SANDBOX` in config.h is 0 (real data) in normal operation.
  **Compliance rule: flip it to 1 (sandbox: fake data, dummy auth) before any
  debugging session where Claude reads serial logs or API responses.** The
  Oura API Agreement (4(d)) forbids feeding real API data to any AI model, so
  Zak's real responses (including scores in serial logs and /api/status) must
  never land in a Claude conversation. Note the sandbox was returning 500s
  for everything except heartrate on 2026-06-09; retest before relying on it.
- OAuth scopes: daily, heartrate, workout, spo2Daily.
- GitHub Pages (privacy policy + ToS, required by the Oura app form) serves
  from `/docs` on main. Attorney-approved; don't edit without flagging.
- **The connected board `/dev/cu.usbserial-2130` is the PLANES board. Never
  flash it from this project.** RingBoard's CYD will get its own device number.
- API budget: 4 calls per refresh, 10 min cadence, 600/day cap in config.h.

A fully working sibling project lives at `~/GitHub/planes/esp32/PlaneBoardDisplay/`
(a live plane tracker on the other board). When in doubt, copy patterns from there: it
has a proven platformio.ini, a flicker-free sprite UI, streamed JSON parsing over TLS,
an in-RAM cache with a daily API budget, and a small WebServer HTTP API that Home
Assistant drives. Its `esp32/README.md` documents the HA YAML.

## The hardware (verified facts, not vendor docs)

- Board: ESP32-2432S028R, 2.8" 240x320 TFT, resistive touch (XPT2046), USB-C,
  CH340 serial, ESP32-D0WD-V3, 4MB flash, **no PSRAM** (~290KB usable heap).
- **The display controller is ST7789, not ILI9341.** The Amazon listing and the
  vendor's own setup guide both say ILI9341; they're wrong for this USB-C (2-USB)
  revision. ILI9341_DRIVER produces portrait-instead-of-landscape, mirrored leftovers
  of the previous framebuffer, and jumbled rendering. The working TFT_eSPI config:
  `ST7789_DRIVER`, `TFT_RGB_ORDER=TFT_BGR`, `TFT_INVERSION_OFF`.
- Pin map (vendor doc, correct): MISO 12, MOSI 13, SCLK 14, CS 15, DC 2, RST -1,
  backlight 21 (HIGH=on), TOUCH_CS 33. Touch is on a *separate* SPI bus
  (CLK 25, MOSI 32, MISO 39, IRQ 36), so TFT_eSPI's same-bus touch won't work; use
  the XPT2046_Touchscreen library if touch is needed.
- Onboard RGB LED on pins 4 (R), 16 (G), 17 (B), **active LOW**, very bright.
- SD slot exists but nothing needs it; 4MB flash with the `huge_app.csv` partition
  scheme (3MB app, no OTA) is the right layout.
- **Upload at 460800 baud.** 921600 (the vendor-recommended speed) fails on the CH340
  with "Unable to verify flash chip connection". macOS has the CH340 driver built in;
  the board appears as /dev/cu.usbserial-XXXX. Zak's first board is usbserial-2130;
  this project's board will get its own number.
- Power: any USB source, but use a USB-A to USB-C cable. C-to-C may not power it
  (many CYD revisions lack the CC pull-down resistors).
- WiFi is **2.4GHz only**. If it won't connect, first suspect a 5GHz-only SSID
  (check exact SSID spelling, including spaces).

## Toolchain

PlatformIO CLI (`pio`), installed via `uv tool install platformio` at
`~/.claude-external/.local/bin`. No Arduino IDE. Inject the TFT_eSPI setup as
build_flags with `-DUSER_SETUP_LOADED=1` so the library is never hand-edited.
Known-good base `platformio.ini` (copied from the planes project, already includes
the driver fix and upload speed):

```ini
[env:cyd]
platform = espressif32@6.9.0
board = esp32dev
framework = arduino
board_build.partitions = huge_app.csv
monitor_speed = 115200
upload_speed = 460800
monitor_filters = esp32_exception_decoder
lib_deps =
    bodmer/TFT_eSPI@2.5.43
    bblanchon/ArduinoJson@^7.2.0
build_flags =
    -DUSER_SETUP_LOADED=1
    -DST7789_DRIVER=1
    -DTFT_RGB_ORDER=TFT_BGR
    -DTFT_INVERSION_OFF=1
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=320
    -DTFT_MISO=12
    -DTFT_MOSI=13
    -DTFT_SCLK=14
    -DTFT_CS=15
    -DTFT_DC=2
    -DTFT_RST=-1
    -DTFT_BL=21
    -DTFT_BACKLIGHT_ON=HIGH
    -DTOUCH_CS=33
    -DLOAD_GLCD=1
    -DLOAD_FONT2=1
    -DLOAD_FONT4=1
    -DLOAD_FONT6=1
    -DLOAD_FONT7=1
    -DLOAD_FONT8=1
    -DLOAD_GFXFF=1
    -DSMOOTH_FONT=1
    -DSPI_FREQUENCY=55000000
    -DSPI_READ_FREQUENCY=20000000
    -DSPI_TOUCH_FREQUENCY=2500000
```

Build/flash/watch:

```bash
pio run
pio run -t upload --upload-port /dev/cu.usbserial-XXXX
pio device monitor -b 115200
```

## Patterns that work on this hardware (from the planes project)

- **Rendering:** no LVGL (RAM-heavy for a no-PSRAM board and overkill for a data
  display). Use TFT_eSPI with one reusable 320x80 16-bit sprite and draw the 240px
  screen as three bands; each `pushSprite` is flicker-free and peak graphics RAM
  stays ~51KB. GFX FreeSans fonts (`setFreeFont(&FreeSansBold24pt7b)` etc.) look
  good; a dark grey background (RGB565(24,24,27)) reads better than pure black.
- **HTTPS APIs:** WiFiClientSecure + `setInsecure()` + HTTPClient with
  `useHTTP10(true)`, then `deserializeJson(doc, http.getStream(), Filter(...))`.
  The filter keeps only needed fields so big responses never sit in RAM. Watch
  free heap in serial logs; the planes board idles at ~174KB free.
- **Secrets:** `src/secrets.h` gitignored, `src/secrets_example.h` tracked. The Oura
  API uses a personal access token, which fits the same pattern.
- **Home Assistant:** an on-device WebServer + ESPmDNS HTTP API
  (`/api/status` JSON, `/api/screen/on|off|auto`) makes HA integration a plain
  `rest` switch/sensor. Pick a unique mDNS name (the plane board took
  `planeboard.local`, so this one needs something else, e.g. `ouracyd.local`).
- **Screen schedule:** NTP via `configTzTime(TZ, "pool.ntp.org", ...)` with
  TZ `PST8PDT,M3.2.0,M11.1.0`, backlight off overnight, HA override wins.
- Metered APIs get an in-RAM cache plus a hard daily call budget; a 24/7 device
  multiplies per-call costs in ways one-shot scripts don't.

## House rules that applied last time

- Swift-style guidelines in `~/.claude/CLAUDE.md` apply to prose, not this C++ code;
  match existing embedded style (snake/camel mix as in the planes firmware).
- Ask before adding third-party libraries beyond TFT_eSPI/ArduinoJson.
- Zak is on a Mac; the board flashes fine from macOS with no extra drivers.
