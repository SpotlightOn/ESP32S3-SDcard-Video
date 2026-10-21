# AGENTS.md

Briefing for coding agents working in this repository. Read it before touching
code; it records the troubleshooting that led to the current state.

## What this project is

The Spotpear **ESP32-S3 1.28" BOX** (round GC9A01 panel, 240×240) turned into a
PlatformIO project. The sketch plays `/video1.mjpeg` from a microSD card in an
endless loop; it is the vendor's "SD card video" demo, converted from the Arduino
IDE package (`main/main.ino` + bundled `libraries/`).

## Hard rules

1. **English only** in code, identifiers, comments, serial output, on-screen
   text, docs and commit messages. The vendor sketch was partly Chinese
   commented; do not reintroduce it.
2. **Libraries come from the PlatformIO registry, never as copies in the repo.**
   `lib_deps` in `platformio.ini` is the single source. The 349 MB vendor
   `libraries/` folder was deliberately deleted; do not re-add `lib_extra_dirs`,
   a `lib/` folder or vendored sources. If a version is missing in the registry,
   pin the closest one and document why in `platformio.ini`.
3. **Do not flash, upload or reset the board unless the user asked for it.**
   Flashing overwrites the state the user is currently testing. Build
   (`pio run`) freely, but ask before `-t upload`.
4. **No unrequested refactoring.** One concrete problem per change, small and
   explainable diffs. If something unrelated looks wrong, report it instead of
   fixing it silently.
5. **No absolute paths and no home directory** in source, comments, docs or
   commit messages - use relative paths or `~`. `.pio/` stays git-ignored
   because the tooling writes absolute paths into it.
6. Keep `README.md` (user facing) and this file in sync when build steps, pins,
   timings or failure messages change.

## Layout

| Path | Purpose |
| --- | --- |
| `platformio.ini` | Single env `spotpear_esp32s3`, registry `lib_deps` |
| `partitions_16mb.csv` | 16 MB flash, 4 MB factory app, rest SPIFFS |
| `src/main.cpp` | Player (was `main/main.ino`) |
| `include/MjpegClass.h` | MJPEG/JPEGDEC wrapper (was `main/MjpegClass.h`) |
| `sdcard/video1.mjpeg` | Sample clip, 290 frames, 25 fps, 11.6 s |
| `tools/display_check/` | Standalone panel + SD test, own `platformio.ini` |
| `opencode.jsonc` | Agent permissions (inherited from the weather clock project) |

## Build, flash, monitor

`pio` is not on `PATH`; use the PlatformIO venv binary:

```bash
~/.platformio/penv/bin/pio run                    # ~25 s, ~9.5 % of the 4 MB slot
~/.platformio/penv/bin/pio run -t upload          # ask the user first
~/.platformio/penv/bin/pio run -d tools/display_check -e check_40m -t upload
```

Serial capture without the monitor (single port open, no DTR/RTS toggling):

```bash
stty -F /dev/ttyACM0 115200 raw -echo && timeout 20 cat /dev/ttyACM0
```

Opening and closing the native USB port in a loop can leave the S3 in download
mode (`boot:0x2 (DOWNLOAD(USB/UART0))`, panel stays dark). Open the port once;
a power cycle recovers it.

## Hardware facts (verified on the device)

* ESP32-S3, **16 MB** flash, **no PSRAM** (`ESP.getPsramSize()` = 0), native USB
  on GPIO19/20 with no UART bridge. Without `-DARDUINO_USB_CDC_ON_BOOT=1`
  nothing reaches the PC.
* Panel: GC9A01, 240×240, IPS (inversion on), MOSI 2, SCLK 4, CS 5, DC 47,
  RST 38, backlight 42.
* Backlight is **active low** (GPIO42 low = on). A lit backlight therefore only
  proves that `setup()` ran, nothing about SPI.
* Panel SPI runs at **40 MHz**, which is the proven value for this board. At
  that clock the player reaches 24.3 fps with 0 skipped frames
  (`read 1 ms, decode 38 ms, show 27 ms`; decode includes the SPI drawing).
  The vendor demo used 80 MHz - do not raise it without measuring.
* SD card: 1-bit SDMMC, CLK 17, CMD 18, D0 21, D3/CS 13 (D3 only driven high as
  CS, as in the vendor demo).

## Firmware behaviour and pitfalls

* `setup()` order: backlight low, `Serial.begin()` + 1.5 s delay (USB CDC has to
  enumerate or the first logs are lost), `gfx->begin(LCD_SPI_HZ)`, then the SD
  mount. The delay is not cosmetic - without it the boot log is empty.
* **The vendor's TFT_eSPI 2.5.0 is unusable on this board.** It defines
  `SPI_PORT` as `FSPI`, which yields register base 0 on the S3, so `tft.begin()`
  panics with `StoreProhibited` (EXCVADDR 0x10) and the panel stays black. That
  crash was the original "dark screen" bug. This sketch uses Arduino_GFX
  instead, whose `Arduino_ESP32SPI` maps `FSPI` to `DR_REG_SPI2_BASE` correctly.
* **Repeated failed `SD_MMC.begin()` calls corrupt the SDMMC host state**: after
  several attempts with no card, a later mount reports
  `SD Card initialized successfully` but every data request times out
  (`sdmmc_host_wait_for_event returned 0x107`). A cold boot fixes it. Hence
  `mountSdCard()` calls `SD_MMC.end()` first, and `loop()` verifies that the
  root directory is readable after a late mount and restarts at most
  `SD_MAX_RESTARTS` times (`RTC_DATA_ATTR` counter, reset after a full pass).
  This recovery path is **not yet verified** with a card inserted at runtime.
* `mount_sd` failures print `sdmmc_init_ocr: send_op_cond (1) returned 0x107`
  (= `ESP_ERR_TIMEOUT`, no card answering). Nothing to debug in the sketch.
* `format_if_mount_failed` is **off**, unlike the vendor demo, so a card with an
  unexpected filesystem is never formatted silently.
* The vendor code reported failures with a small `gfx->println()` at the
  origin, which is easy to overlook on a dark 240×240 panel. `showMessage()`
  draws a readable full-screen message instead (`NO SD CARD`,
  `NO VIDEO FILE`, `OUT OF MEMORY`).
* `MjpegClass` reads `/video1.mjpeg` from the card root. Vendor content lives in
  a `sdcard/` folder on the card and has to be copied to the root.
* The frame pacing is the vendor's: a schedule based on
  `start_ms + frame * 1000 / FPS` with `FPS 25`, so it catches up by skipping
  frames. Per-pass statistics are printed once instead of a line per skipped
  frame.
* Registry versions: Arduino_GFX `1.4.9` (vendor version), JPEGDEC `1.8.4`
  (vendor had 1.6.1, which is not published; 1.8.4 keeps the API).

## Conventions

* One logical change per commit; the message is a single short English sentence
  with no body.
* Never commit `.pio/` or `*.bin`; `.gitignore` covers them.
