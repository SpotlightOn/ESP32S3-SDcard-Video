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

## Creating the video

The player reads a raw **Motion JPEG** stream: baseline JPEG frames concatenated
one after another, no container metadata, no audio. Timing is not read from the
file — the firmware shows 25 frames per second (`FPS` in `src/main.cpp`), so a
clip encoded at 25 fps plays at its original speed.

| Requirement | Value |
| --- | --- |
| File name | `video1.mjpeg` in the **root** of the card (`/video1.mjpeg`) |
| File system | FAT32 |
| Stream | MJPEG (Motion JPEG), audio stripped |
| Frames | baseline JPEG, `yuvj420p` (4:2:0) |
| Resolution | 240×240; the firmware only scales by 1/2, 1/4 or 1/8, so do not rely on it to fit other sizes |
| Frame rate | 25 fps, fixed in the firmware |
| Frame budget | one JPEG frame must stay below 115200 bytes (`MJPEG_BUFFER_SIZE`); 240×240 uses ~5–20 KB |

The `sdcard/` folder in this repository is only a staging copy — the card itself
needs the file in its root.

### From a normal video

```bash
ffmpeg -i input.mp4 \
  -vf "scale=240:240:force_original_aspect_ratio=decrease,pad=240:240:(ow-iw)/2:(oh-ih)/2" \
  -r 25 -c:v mjpeg -q:v 4 -an -f mjpeg video1.mjpeg
```

* `scale`/`pad` fit any aspect ratio into the square panel, black bars included.
* `-r 25` resamples the frame rate to what the player expects.
* `-q:v 4` is the JPEG quality (2 = best, 31 = worst); at 240×240 every setting
  stays far below the 115 KB frame budget.
* `-an` drops the audio track. Audio data between the frames contains arbitrary
  bytes, including `FF D8`/`FF D9` pairs, which the frame scanner would treat as
  picture data.
* `-f mjpeg` writes a pure MJPEG stream instead of a single JPEG.

### From a single picture (static image)

```bash
ffmpeg -loop 1 -i photo.jpg -t 10 -r 25 \
  -vf "scale=240:240:force_original_aspect_ratio=decrease,pad=240:240:(ow-iw)/2:(oh-ih)/2" \
  -c:v mjpeg -q:v 4 -an -f mjpeg video1.mjpeg
```

`-t 10` repeats the picture for 10 seconds; the player then loops it.

### From a folder of pictures (slideshow)

```bash
cat > list.txt <<'EOF'
file 'img1.jpg'
duration 3
file 'img2.jpg'
duration 3
file 'img3.jpg'
duration 3
EOF

ffmpeg -f concat -safe 0 -i list.txt \
  -vf "scale=240:240:force_original_aspect_ratio=decrease,pad=240:240:(ow-iw)/2:(oh-ih)/2,format=yuvj420p" \
  -r 25 -c:v mjpeg -q:v 4 -an -f mjpeg video1.mjpeg
```

Each `duration` is the time that picture is shown.

### Check the result before copying it

```bash
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -show_entries format=format_name -of default=noprint_wrappers=1 video1.mjpeg
```

Expected: `codec_name=mjpeg`, `width=240`, `height=240`, `avg_frame_rate=25/1`.
`format_name` shows `mjpeg` or `jpeg_pipe` depending on how ffprobe guesses; both
mean the same raw stream of JPEGs and both play.

Then copy it to the card root and flush the write cache:

```bash
cp video1.mjpeg /media/$USER/<card>/video1.mjpeg
sync
```

Only one clip is supported; there is no playlist. The sample in `sdcard/` has
290 frames at 25 fps (11.6 s).

### Pictures without converting them

There is no image viewer: `/image.jpg` is not read, only `/video1.mjpeg`. A
still picture works through the `-loop 1` recipe above. A real image mode (show
one JPEG and stop) can be added if it is needed.

## Board facts (verified on the device)

* ESP32-S3, **16 MB** flash, no PSRAM. 4 MB single application slot without OTA.
* Native USB on GPIO19/20, no USB-UART bridge: without
  `-DARDUINO_USB_CDC_ON_BOOT=1` nothing reaches the PC.
* Panel: GC9A01, 240×240, IPS (inversion on), pins MOSI 2, SCLK 4, CS 5,
  DC 47, RST 38, backlight 42.
* Backlight is **active low** (GPIO42 low = on).
* Panel SPI runs at **40 MHz**, verified on this board with this sketch
  (24.3 fps, 0 skipped frames). The vendor video demo used 80 MHz; that was
  never confirmed on this board, so 40 MHz is the default. Override with
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
