/***
 * MJPEG video player for the Spotpear ESP32-S3 1.28" BOX board (round GC9A01
 * LCD). Plays /video1.mjpeg from the SD card in an endless loop.
 *
 * Required libraries:
 * Arduino_GFX: https://github.com/moononournation/Arduino_GFX.git
 * JPEGDEC:     https://github.com/bitbank2/JPEGDEC.git
 *
 * Both are pulled from the PlatformIO registry (see platformio.ini).
 */
#include <Arduino.h>

#define MJPEG_FILENAME "/video1.mjpeg"
#define FPS 25
#define MJPEG_BUFFER_SIZE (240 * 240 * 4 / 2)

// Panel SPI clock. 40 MHz is verified on this board (24.3 fps, 0 skipped
// frames); the vendor demo used 80 MHz, which was never measured here.
// Override with -DLCD_SPI_HZ=...
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 40000000
#endif

#define SDMMC_D3 13  // SDMMC Data3 / SPI CS
#define SDMMC_CMD 18 // SDMMC CMD   / SPI MOSI
#define SDMMC_CLK 17 // SDMMC CLK   / SPI SCK
#define SDMMC_D0 21  // SDMMC Data0 / SPI MISO

#define TFT_MOSI 2  // 1.90 4  1.44  4
#define TFT_SCLK 4  // 1.90 11  1.44 3
#define TFT_CS 5    // Chip select control pin  1.90 12  1.44 2
#define TFT_DC 47   // Data Command control pin   1.90 7  1.44 0
#define TFT_RST 38
#define LCD_BL 42

#include <WiFi.h>
#include <FS.h>

#include <FFat.h>
#include <LittleFS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPIFFS.h>

/*******************************************************************************
 * Start of Arduino_GFX setting
 ******************************************************************************/
#include <Arduino_GFX_Library.h>

/* More data bus class: https://github.com/moononournation/Arduino_GFX/wiki/Data-Bus-Class */
Arduino_DataBus *bus = new Arduino_ESP32SPI(47 /* DC */, 5 /* CS */, 4 /* SCK */, 2 /* MOSI */, GFX_NOT_DEFINED /* MISO */, FSPI /* spi_num */);
/* More display class: https://github.com/moononournation/Arduino_GFX/wiki/Display-Class */
// Arduino_GFX *gfx = new Arduino_ST7789(bus, 42 /* RST */, 4 /* rotation */, true /* IPS */, 240 /* width */, 240 /* height */, 0 /* col offset 1 */, 85 /* row offset 1 */, 0 /* col offset 2 */, 0 /* row offset 2 */);
// Arduino_GFX *gfx = new Arduino_ST7789(bus, 38 /* RST */, 0 /* rotation */, true /* IPS */, 240 /* width */, 240 /* height */, 0 /* col offset 1 */, 0 /* row offset 1 */, 0 /* col offset 2 */, 0 /* row offset 2 */);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 38 /*42 RST */, 3 /* rotation */, true /* IPS */, 240 /* width */, 240 /* height */, 0 /* col offset 1 */, 0 /* row offset 1 */, 0 /* col offset 2 */, 0 /* row offset 2 */);
/*******************************************************************************
 * End of Arduino_GFX setting
 ******************************************************************************/

/* variables */
static int next_frame = 0;
static int skipped_frames = 0;
static unsigned long start_ms, curr_ms, next_frame_ms;
static unsigned long total_read_video_ms = 0;
static unsigned long total_decode_video_ms = 0;
static unsigned long total_show_video_ms = 0;

/* MJPEG Video */
#include "MjpegClass.h"
static MjpegClass mjpeg;

// pixel drawing callback
static int drawMCU(JPEGDRAW *pDraw)
{
  unsigned long s = millis();
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  total_show_video_ms += millis() - s;

  return 1;
} /* drawMCU() */

// Show a message on the panel instead of leaving it black, so a failure is
// recognisable without a serial connection.
static void showMessage(const char *line1, const char *line2)
{
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 90);
  gfx->println(line1);
  if (line2)
  {
    gfx->setTextSize(2);
    gfx->setCursor(20, 130);
    gfx->println(line2);
  }
}

// Frame statistics for the serial log
static unsigned long frames_shown = 0;
static unsigned long frames_skipped = 0;

// Survives a software restart, so the card recovery below cannot loop forever
RTC_DATA_ATTR static uint8_t sd_restarts = 0;
#define SD_MAX_RESTARTS 2

static bool mountSdCard()
{
  // Tear down a previous attempt first: several failed begin() calls in a row
  // leave the SDMMC host in a state where a later mount succeeds but every
  // data request times out.
  SD_MMC.end();

  pinMode(SDMMC_D3 /* CS */, OUTPUT);
  digitalWrite(SDMMC_D3 /* CS */, HIGH);
  SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);

  // format_if_mount_failed stays off on purpose: the vendor demo formatted the
  // card, which would erase the video and everything else on it.
  if (!SD_MMC.begin("/sdcard", true /* mode1bit */, false /* format if mount failed */, 8000000))
  {
    Serial.println("Card Mount Failed");
    return false;
  }
  Serial.println("SD Card initialized successfully");
  return true;
}

// A card that was inserted while earlier mount attempts were failing can mount
// but no longer answer data requests. One clean restart repairs that for good,
// so check that the root directory can actually be read.
static bool sdCardIsReadable()
{
  File root = SD_MMC.open("/");
  bool readable = root && root.isDirectory();
  if (root)
  {
    root.close();
  }
  return readable;
}

// Pixel buffer for one MJPEG frame. Allocated once: allocating and freeing
// ~115 KB per loop fragments the heap and eventually makes the allocation fail,
// which is what produced the late "OUT OF MEMORY" message.
static uint8_t *mjpeg_buf = nullptr;

static bool playVideo()
{
  File vFile = SD_MMC.open(MJPEG_FILENAME);
  if (!vFile || vFile.isDirectory())
  {
    Serial.println("ERROR: Failed to open " MJPEG_FILENAME " file for reading");
    showMessage("NO VIDEO FILE", MJPEG_FILENAME " missing on card");
    return false;
  }

  if (mjpeg_buf == nullptr)
  {
    mjpeg_buf = (uint8_t *)malloc(MJPEG_BUFFER_SIZE);
    if (mjpeg_buf == nullptr)
    {
      Serial.printf("mjpeg_buf malloc failed, %u bytes free, largest block %u\n",
                    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
      vFile.close();
      showMessage("OUT OF MEMORY", "mjpeg buffer");
      return false;
    }
  }

  if (!mjpeg.setup(
          &vFile, mjpeg_buf, MJPEG_BUFFER_SIZE, drawMCU, true /* useBigEndian */,
          0 /* x */, 0 /* y */, gfx->width() /* widthLimit */, gfx->height() /* heightLimit */))
  {
    Serial.println("mjpeg.setup failed, out of memory");
    vFile.close();
    showMessage("OUT OF MEMORY", "mjpeg read buffer");
    return false;
  }

  start_ms = millis();
  curr_ms = millis();
  next_frame_ms = start_ms + (++next_frame * 1000 / FPS / 2);

  while (vFile.available() && mjpeg.readMjpegBuf()) // Read video
  {
    total_read_video_ms += millis() - curr_ms;
    curr_ms = millis();

    if (millis() < next_frame_ms) // show frame or skip frame
    {
      // Play video
      mjpeg.drawJpg();
      ++frames_shown;
      total_decode_video_ms += millis() - curr_ms;
      curr_ms = millis();
    }
    else
    {
      ++skipped_frames;
    }

    while (millis() < next_frame_ms)
    {
      vTaskDelay(pdMS_TO_TICKS(1));
    }

    curr_ms = millis();
    next_frame_ms = start_ms + (++next_frame * 1000 / FPS);
  }

  // Clean up
  vFile.close();

  unsigned long elapsed = millis() - start_ms;
  Serial.printf("played %lu frames in %lu ms (%lu skipped), %.1f fps, read %lu ms, decode %lu ms, show %lu ms per frame, heap %u, largest block %u\n",
                frames_shown, elapsed, frames_skipped,
                elapsed ? (float)frames_shown * 1000.0f / (float)elapsed : 0.0f,
                frames_shown ? total_read_video_ms / frames_shown : 0,
                frames_shown ? total_decode_video_ms / frames_shown : 0,
                frames_shown ? total_show_video_ms / frames_shown : 0,
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // The frame scanner stopped because a single frame does not fit into the
  // mjpeg buffer, so the clip cannot be played at all.
  if (mjpeg.frameTooLarge())
  {
    showMessage("VIDEO TOO LARGE", "use 240x240 JPG frames");
    return false;
  }

  return true;
}

void setup()
{
  // WiFi.mode(WIFI_OFF);
  // gfx->setRotation(0);
  Serial.begin(115200);
  delay(1500); // give the USB CDC port time to enumerate, otherwise early logs are lost

  Serial.printf("free heap at boot: %u bytes\n", (unsigned)ESP.getFreeHeap());

#ifdef GFX_EXTRA_PRE_INIT
  GFX_EXTRA_PRE_INIT();
#endif

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW); // backlight is active low on this board

  Serial.printf("Init display, SPI %u Hz\n", (unsigned)LCD_SPI_HZ);
  if (!gfx->begin(LCD_SPI_HZ))
  {
    Serial.println("Init display failed!");
  }
  delay(100);
  gfx->fillScreen(BLACK);

  if (!mountSdCard())
  {
    showMessage("NO SD CARD", "insert a FAT32 card with " MJPEG_FILENAME);
  }
}

void loop()
{
  // Wait for the SD card, so the video starts as soon as one is inserted
  if (!SD_MMC.cardType())
  {
    delay(2000);
    if (!mountSdCard())
    {
      return;
    }

    gfx->fillScreen(BLACK);

    if (!sdCardIsReadable() && sd_restarts < SD_MAX_RESTARTS)
    {
      sd_restarts++;
      Serial.printf("SD card mounts but cannot be read, restarting (%u/%u)\n",
                    sd_restarts, SD_MAX_RESTARTS);
      Serial.flush();
      delay(100);
      ESP.restart();
    }
  }

  if (!playVideo())
  {
    delay(2000);
    return;
  }

  // Full pass played, so the card is healthy again
  sd_restarts = 0;

  // Reset the frame counters
  next_frame = 0;
  skipped_frames = 0;
  frames_shown = 0;
  frames_skipped = 0;
  total_read_video_ms = 0;
  total_decode_video_ms = 0;
  total_show_video_ms = 0;

  // Short pause between loops so the SD card can settle
  delay(50);
}
