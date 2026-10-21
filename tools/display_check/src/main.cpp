// Panel and SD card check for the Spotpear ESP32-S3 1.28" BOX board.
//
// Runs the same display driver path as the SD video player (Arduino_GFX,
// GC9A01, SPI on GPIO 4/2/5/47/38, backlight on GPIO 42) and then mounts the
// SD card in 1-bit SD_MMC mode. Every step is reported on Serial, and the
// panel shows full-screen colours so "dark screen" can be told apart from
// "black background with a single line of error text".

#include <Arduino.h>
#include <SD_MMC.h>

#include <Arduino_GFX_Library.h>

#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 40000000
#endif

// Backlight of this board is active low (GPIO42 low = on)
static const int PIN_BACKLIGHT = 42;

// SD card in 1-bit SD_MMC mode, same pins as the vendor video demo
static const int SD_CLK = 17;
static const int SD_CMD = 18;
static const int SD_D0 = 21;
static const int SD_D3 = 13;

Arduino_DataBus *bus = new Arduino_ESP32SPI(47 /* DC */, 5 /* CS */, 4 /* SCK */, 2 /* MOSI */, GFX_NOT_DEFINED /* MISO */, FSPI /* spi_num */);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 38 /* RST */, 3 /* rotation */, true /* IPS */, 240, 240, 0, 0, 0, 0);

static bool g_spiOk = false;
static bool g_sdOk = false;

static void logf(const char *fmt, ...)
{
  char buf[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  Serial.flush();
}

void setup()
{
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, LOW);

  Serial.begin(115200);
  delay(1500); // let the USB CDC port enumerate

  logf("[check] start, SPI %u Hz", (unsigned)LCD_SPI_HZ);
  logf("[check] free heap %u bytes", (unsigned)ESP.getFreeHeap());

  bool ok = gfx->begin(LCD_SPI_HZ);
  g_spiOk = ok;
  logf("[check] gfx->begin() -> %s", ok ? "ok" : "failed");
  logf("[check] panel %d x %d", gfx->width(), gfx->height());

  // Full screen colours, each labelled so the serial log and the panel match
  const uint16_t colours[4] = {RED, GREEN, BLUE, WHITE};
  const char *names[4] = {"RED", "GREEN", "BLUE", "WHITE"};

  for (int i = 0; i < 4; i++)
  {
    gfx->fillScreen(colours[i]);
    gfx->setTextColor((i == 3) ? BLACK : WHITE, colours[i]);
    gfx->setTextSize(3);
    gfx->setCursor(20, 90);
    gfx->printf("%s\n", names[i]);
    gfx->setTextSize(2);
    gfx->setCursor(20, 130);
    gfx->printf("%u MHz\n%d x %d", (unsigned)(LCD_SPI_HZ / 1000000), gfx->width(), gfx->height());
    logf("[check] filled %s", names[i]);
    delay(2000);
  }

  // Backlight polarity check: same picture, backlight off then on
  gfx->fillScreen(MAGENTA);
  logf("[check] backlight OFF for 2 s");
  digitalWrite(PIN_BACKLIGHT, HIGH);
  delay(2000);
  logf("[check] backlight ON for 2 s");
  digitalWrite(PIN_BACKLIGHT, LOW);
  delay(2000);

  // SD card
  pinMode(SD_D3 /* CS */, OUTPUT);
  digitalWrite(SD_D3 /* CS */, HIGH);
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  logf("[check] mounting SD card");
  if (!SD_MMC.begin("/sdcard", true /* mode1bit */, true /* format if mount failed */, 8000000))
  {
    logf("[check] SD mount FAILED");
    gfx->fillScreen(BLACK);
    gfx->setTextColor(RED);
    gfx->setTextSize(3);
    gfx->setCursor(30, 100);
    gfx->println("NO SD");
  }
  else
  {
    uint8_t cardType = SD_MMC.cardType();
    g_sdOk = true;
    logf("[check] SD mounted, type %u, size %llu MB",
         cardType, SD_MMC.cardSize() / (1024ULL * 1024ULL));

    File root = SD_MMC.open("/");
    if (root && root.isDirectory())
    {
      File f = root.openNextFile();
      int shown = 0;
      while (f && shown < 10)
      {
        logf("[check]   %s%s  %u bytes", f.isDirectory() ? "/" : "", f.name(), (unsigned)f.size());
        f = root.openNextFile();
        shown++;
      }
      if (shown == 0)
      {
        logf("[check]   <root is empty>");
      }
    }

    File video = SD_MMC.open("/video1.mjpeg");
    logf("[check] /video1.mjpeg -> %s", (video && !video.isDirectory()) ? "found" : "missing");
    if (video)
    {
      logf("[check]   size %u bytes", (unsigned)video.size());
      video.close();
    }

    gfx->fillScreen(BLACK);
    gfx->setTextColor(GREEN);
    gfx->setTextSize(3);
    gfx->setCursor(30, 100);
    gfx->println("SD OK");
  }

  logf("[check] setup finished");
}

void loop()
{
  static uint32_t last = 0;
  static int x = 0;

  if (millis() - last > 1000)
  {
    last = millis();
    logf("[check] alive %lu s, %u MHz begin=%s sd=%s, free heap %u",
         (unsigned long)(millis() / 1000), (unsigned)(LCD_SPI_HZ / 1000000),
         g_spiOk ? "ok" : "failed", g_sdOk ? "ok" : "failed",
         (unsigned)ESP.getFreeHeap());

    // Moving bar, so a frozen panel is visible as well
    gfx->fillRect(0, 220, 240, 20, BLACK);
    gfx->fillRect(x, 220, 20, 20, CYAN);
    x = (x + 20) % 240;
  }
}
