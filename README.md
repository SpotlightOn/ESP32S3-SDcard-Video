# SDcard-Video — ESP32-S3 1.28" BOX MJPEG player (PlatformIO)

Plays `video1.mjpeg` from the microSD card in an endless loop on the round
GC9A01 display (240×240) of the Spotpear ESP32-S3 1.28" BOX board.

## Layout

```
platformio.ini                     build configuration
partitions_16mb.csv                16 MB flash layout, 4 MB application slot
src/main.cpp                       application (was main/main.ino)
include/MjpegClass.h               MJPEG/JPEGDEC wrapper (was main/MjpegClass.h)
sdcard/video1.mjpeg                sample video, copy to the SD card root
tools/display_check/               standalone panel + SD test, own platformio.ini
```

Libraries come from the PlatformIO registry (`lib_deps`), so no library copies
live in this folder. `pio run` installs them into `.pio/libdeps/` on first use.

## Build, flash, monitor

`pio` is usually not on `PATH`; use the PlatformIO venv binary:

```bash
~/.platformio/penv/bin/pio run                 # compile
~/.platformio/penv/bin/pio run -t upload       # flash
~/.platformio/penv/bin/pio device monitor      # serial, 115200 baud
```

Serial capture without the monitor (single port open):

```bash
stty -F /dev/ttyACM0 115200 raw -echo && timeout 20 cat /dev/ttyACM0
```

Opening and closing the native USB port in a loop can leave the S3 in download
mode (`boot:0x2 (DOWNLOAD(USB/UART0))`); open the port once and power cycle if it
gets stuck.

## SD card

Copy `sdcard/video1.mjpeg` to the root of a FAT32 microSD card, so the player
finds `/video1.mjpeg`. The card is used in 1-bit SD_MMC mode: CLK 17, CMD 18,
D0 21, D3/CS 13.

The player mounts the card again every 2 s, so inserting it later starts the
video without a reset. If mounting keeps failing, the serial log shows
`sdmmc_init_ocr: send_op_cond (1) returned 0x107` — that is `ESP_ERR_TIMEOUT`,
i.e. no card is answering (empty slot, dirty contacts, or a card that needs a
different format).

`format_if_mount_failed` is deliberately off, unlike the vendor demo, so a card
with an unexpected filesystem is never formatted silently.

## Board facts (verified on the device)

* ESP32-S3, **16 MB** flash, no PSRAM. 4 MB single application slot without OTA.
* Native USB on GPIO19/20, no USB-UART bridge: without
  `-DARDUINO_USB_CDC_ON_BOOT=1` nothing reaches the PC.
* Panel: GC9A01, 240×240, IPS (inversion on), pins MOSI 2, SCLK 4, CS 5,
  DC 47, RST 38, backlight 42.
* Backlight is **active low** (GPIO42 low = on).
* Panel SPI runs at **40 MHz**, the value the working weather clock firmware for
  this board uses. The vendor video demo used 80 MHz; that was never confirmed
  to light the panel here, so 40 MHz is the default. Override with
  `-DLCD_SPI_HZ=80000000`.
* The display is initialised with Arduino_GFX, which is not affected by the
  TFT_eSPI bug of this vendor bundle (2.5.0 defines `SPI_PORT` as `FSPI`, which
  yields register base 0 on the S3 and panics in `tft.begin()`).

## Tools

```bash
~/.platformio/penv/bin/pio run -d tools/display_check -e check_40m -t upload
~/.platformio/penv/bin/pio run -d tools/display_check -e check_80m -t upload
```

`display_check` cycles full-screen colours with labels, toggles the backlight,
mounts the SD card and lists the root directory. It separates "panel dead" from
"panel fine, black background with unnoticed error text". The envs differ only in
the SPI clock.

## Failure messages on the panel

Instead of a black screen the player now shows a readable message:

| Message | Meaning |
| --- | --- |
| `NO SD CARD` | `SD_MMC.begin()` failed, no card detected |
| `NO VIDEO FILE` | card mounted, but `/video1.mjpeg` is missing |
| `OUT OF MEMORY` | the ~115 KB MJPEG buffer could not be allocated |
