// ESP32-8048S070 7.0" MJPEG movie player — autoplay loop from SPI SD card.
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <Arduino_GFX_Library.h>

#include "app_config.h"
#include "MjpegClass.h"

// ---------------------------------------------------------------------------
// Display (Arduino_GFX RGB panel — ESP32-8048S070 Profile B)
// ---------------------------------------------------------------------------

Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    GFX_NOT_DEFINED /* CS */, GFX_NOT_DEFINED /* SCK */, GFX_NOT_DEFINED /* SDA */,
    LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK,
    LCD_PIN_R0, LCD_PIN_R1, LCD_PIN_R2, LCD_PIN_R3, LCD_PIN_R4,
    LCD_PIN_G0, LCD_PIN_G1, LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5,
    LCD_PIN_B0, LCD_PIN_B1, LCD_PIN_B2, LCD_PIN_B3, LCD_PIN_B4);

Arduino_GFX *gfx = new Arduino_RPi_DPI_RGBPanel(
    bus,
    LCD_WIDTH, LCD_HSYNC_POLARITY, LCD_HSYNC_FRONT_PORCH, LCD_HSYNC_PULSE_WIDTH, LCD_HSYNC_BACK_PORCH,
    LCD_HEIGHT, LCD_VSYNC_POLARITY, LCD_VSYNC_FRONT_PORCH, LCD_VSYNC_PULSE_WIDTH, LCD_VSYNC_BACK_PORCH,
    LCD_PCLK_ACTIVE_NEG, LCD_PCLK_HZ, false /* auto_flush — flush once per frame */);

// ---------------------------------------------------------------------------
// MJPEG playback state
// ---------------------------------------------------------------------------

static MjpegClass mjpeg;
static File mjpegFile;
static uint8_t *mjpegBuffer = nullptr;
static bool sdCardReady = false;

// Dual-core pipeline: the reader task fills frame slots, the loop core decodes.
static uint8_t *frameSlots[MJPEG_FRAME_SLOT_COUNT] = {nullptr};
static int32_t frameSlotLen[MJPEG_FRAME_SLOT_COUNT] = {0};
static QueueHandle_t freeSlotQueue = nullptr;   // indices ready to be filled
static QueueHandle_t filledSlotQueue = nullptr; // indices holding a frame
static const int kReaderEofSentinel = -1;

enum MovieScanResult
{
  MOVIE_SCAN_PLAYED,
  MOVIE_SCAN_NO_CARD,
  MOVIE_SCAN_NO_FOLDER,
  MOVIE_SCAN_NO_MOVIES
};

static void initBacklight()
{
  ledcSetup(0, 600, 8);
  ledcAttachPin(TFT_BL, 0);
  ledcWrite(0, 255);
}

static void showStatus(const char *line1, const char *line2 = nullptr)
{
  gfx->fillScreen(BLACK);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(20, LCD_HEIGHT / 2 - 30);
  gfx->println(line1);
  if (line2 != nullptr)
  {
    gfx->println(line2);
  }
  gfx->flush();
}

static bool pathEndsWith(const char *path, const char *suffix)
{
  if (path == nullptr || suffix == nullptr)
  {
    return false;
  }

  size_t pathLen = strlen(path);
  size_t suffixLen = strlen(suffix);
  if (pathLen < suffixLen)
  {
    return false;
  }

  const char *p = path + pathLen - suffixLen;
  while (*p && *suffix)
  {
    char a = *p++;
    char b = *suffix++;
    if (a >= 'A' && a <= 'Z')
    {
      a = (char)(a + ('a' - 'A'));
    }
    if (b >= 'A' && b <= 'Z')
    {
      b = (char)(b + ('a' - 'A'));
    }
    if (a != b)
    {
      return false;
    }
  }
  return true;
}

static bool mediaPathIsMjpeg(const char *path)
{
  return pathEndsWith(path, ".mjpeg") || pathEndsWith(path, ".mjpg");
}

static bool initSDCard()
{
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  if (!SD.begin(SD_CS, SPI, SD_SPI_FREQ_HZ))
  {
    Serial.println("SD.begin() failed");
    sdCardReady = false;
    return false;
  }

  sdCardReady = true;
  Serial.println("SD card mounted");
  return true;
}

static bool ensureSDCardReady()
{
  if (sdCardReady)
  {
    return true;
  }

  if (initSDCard())
  {
    showStatus("SD CARD READY", "SCANNING /mjpeg");
    return true;
  }

  showStatus("INSERT SD CARD", "WAITING FOR MEDIA");
  delay(MJPEG_RETRY_DELAY_MS);
  return false;
}

static bool initMjpegBuffer()
{
  if (mjpegBuffer != nullptr)
  {
    return true;
  }

  mjpegBuffer = (uint8_t *)heap_caps_malloc(MJPEG_BUFFER_SIZE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (mjpegBuffer != nullptr)
  {
    Serial.println("MJPEG buffer allocated in PSRAM");
    return true;
  }

  mjpegBuffer = (uint8_t *)malloc(MJPEG_BUFFER_SIZE_BYTES);
  if (mjpegBuffer == nullptr)
  {
    Serial.println("MJPEG buffer alloc failed");
    return false;
  }

  Serial.println("MJPEG buffer allocated in internal RAM (fallback)");
  return true;
}

static bool initFrameSlots()
{
  if (freeSlotQueue != nullptr && filledSlotQueue != nullptr)
  {
    return true;
  }

  for (int i = 0; i < MJPEG_FRAME_SLOT_COUNT; ++i)
  {
    if (frameSlots[i] == nullptr)
    {
      frameSlots[i] = (uint8_t *)heap_caps_malloc(MJPEG_FRAME_SLOT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (frameSlots[i] == nullptr)
      {
        Serial.println("Frame slot alloc failed");
        return false;
      }
    }
  }

  // filled queue needs room for every slot plus the EOF sentinel.
  freeSlotQueue = xQueueCreate(MJPEG_FRAME_SLOT_COUNT, sizeof(int));
  filledSlotQueue = xQueueCreate(MJPEG_FRAME_SLOT_COUNT + 1, sizeof(int));
  if (freeSlotQueue == nullptr || filledSlotQueue == nullptr)
  {
    Serial.println("Frame slot queue alloc failed");
    return false;
  }

  Serial.println("Frame slots allocated in PSRAM");
  return true;
}

// Producer (pinned to MJPEG_READER_TASK_CORE): pulls complete JPEG frames off
// the SD card and hands them to the decode core through the slot ring.
static void mjpegReaderTask(void *param)
{
  while (mjpegFile.available())
  {
    if (!mjpeg.readMjpegBuf())
    {
      break;
    }

    int32_t len = mjpeg.frameLen();
    if (len <= 0)
    {
      continue;
    }

    int slot = 0;
    if (xQueueReceive(freeSlotQueue, &slot, portMAX_DELAY) != pdTRUE)
    {
      break;
    }

    if (len > (int32_t)MJPEG_FRAME_SLOT_BYTES)
    {
      // Frame larger than a slot — drop it rather than overflow.
      Serial.printf("Frame too big for slot (%ld bytes), skipping\n", (long)len);
      xQueueSend(freeSlotQueue, &slot, portMAX_DELAY);
      continue;
    }

    memcpy(frameSlots[slot], mjpeg.frameData(), (size_t)len);
    frameSlotLen[slot] = len;
    xQueueSend(filledSlotQueue, &slot, portMAX_DELAY);
  }

  int sentinel = kReaderEofSentinel;
  xQueueSend(filledSlotQueue, &sentinel, portMAX_DELAY);
  vTaskDelete(nullptr);
}

static int jpegDrawCallback(JPEGDRAW *pDraw)
{
  gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
  return 1;
}

static bool playMjpegOnce(const char *path)
{
  if (!initMjpegBuffer() || !initFrameSlots())
  {
    return false;
  }

  mjpegFile = SD.open(path, FILE_READ);
  if (!mjpegFile || mjpegFile.isDirectory())
  {
    Serial.print("mjpeg open failed: ");
    Serial.println(path);
    return false;
  }

  if (!mjpeg.setup(
          &mjpegFile,
          mjpegBuffer,
          MJPEG_BUFFER_SIZE_BYTES,
          jpegDrawCallback,
          MJPEG_USE_BIG_ENDIAN,
          0,
          0,
          LCD_WIDTH,
          LCD_HEIGHT))
  {
    Serial.println("mjpeg.setup() failed");
    mjpegFile.close();
    return false;
  }

  gfx->fillScreen(BLACK);
  gfx->flush();

  // Reset the slot ring: every slot starts free, no frames pending.
  xQueueReset(freeSlotQueue);
  xQueueReset(filledSlotQueue);
  for (int i = 0; i < MJPEG_FRAME_SLOT_COUNT; ++i)
  {
    xQueueSend(freeSlotQueue, &i, 0);
  }

  TaskHandle_t readerTask = nullptr;
  if (xTaskCreatePinnedToCore(
          mjpegReaderTask, "mjpegReader", MJPEG_READER_TASK_STACK, nullptr,
          MJPEG_READER_TASK_PRIORITY, &readerTask, MJPEG_READER_TASK_CORE) != pdPASS)
  {
    Serial.println("reader task create failed");
    mjpegFile.close();
    return false;
  }

  uint32_t frameCount = 0;
  uint32_t startMs = millis();

  uint64_t waitUs = 0;   // blocked waiting for a frame => reader-bound
  uint64_t decodeUs = 0; // JPEG decode + draw into framebuffer
  uint64_t flushUs = 0;  // cache writeback of the framebuffer

  while (true)
  {
    int slot = 0;
    uint32_t t0 = micros();
    if (xQueueReceive(filledSlotQueue, &slot, portMAX_DELAY) != pdTRUE)
    {
      break;
    }
    uint32_t t1 = micros();

    if (slot == kReaderEofSentinel)
    {
      break;
    }

    mjpeg.drawJpgBuffer(frameSlots[slot], frameSlotLen[slot], LCD_WIDTH, LCD_HEIGHT);
    uint32_t t2 = micros();

    gfx->flush();
    uint32_t t3 = micros();

    xQueueSend(freeSlotQueue, &slot, portMAX_DELAY);

    waitUs += (uint64_t)(t1 - t0);
    decodeUs += (uint64_t)(t2 - t1);
    flushUs += (uint64_t)(t3 - t2);

    frameCount++;
    yield();
  }

  mjpegFile.close();

  uint32_t elapsedMs = millis() - startMs;
  float fps = (elapsedMs > 0) ? (frameCount * 1000.0f / (float)elapsedMs) : 0.0f;

  Serial.print("Finished: ");
  Serial.print(path);
  Serial.print(" frames=");
  Serial.print(frameCount);
  Serial.print(" fps=");
  Serial.println(fps, 2);

  if (frameCount > 0)
  {
    // wait>0 means the decode core sat idle waiting on the reader (I/O-bound);
    // wait~0 means decode is the wall (compute-bound) and the pipeline is full.
    Serial.print("  avg/frame (ms): wait=");
    Serial.print((float)waitUs / frameCount / 1000.0f, 2);
    Serial.print(" decode+draw=");
    Serial.print((float)decodeUs / frameCount / 1000.0f, 2);
    Serial.print(" flush=");
    Serial.println((float)flushUs / frameCount / 1000.0f, 2);
  }

  return frameCount > 0;
}

static MovieScanResult playMoviesFromDirectory(const char *directoryPath)
{
  File directory = SD.open(directoryPath);
  if (!directory || !directory.isDirectory())
  {
    Serial.print("Directory open failed: ");
    Serial.println(directoryPath);
    directory.close();
    return MOVIE_SCAN_NO_FOLDER;
  }

  bool foundMovie = false;
  directory.rewindDirectory();

  while (true)
  {
    File entry = directory.openNextFile();
    if (!entry)
    {
      break;
    }

    if (entry.isDirectory())
    {
      entry.close();
      continue;
    }

    const char *entryPath = entry.path();
    if (entryPath == nullptr || entryPath[0] == '\0')
    {
      entryPath = entry.name();
    }

    String moviePath = (entryPath != nullptr) ? String(entryPath) : String();
    entry.close();

    if (moviePath.length() == 0 || !mediaPathIsMjpeg(moviePath.c_str()))
    {
      continue;
    }

    foundMovie = true;
    Serial.print("Playing: ");
    Serial.println(moviePath.c_str());
    playMjpegOnce(moviePath.c_str());
  }

  directory.close();
  return foundMovie ? MOVIE_SCAN_PLAYED : MOVIE_SCAN_NO_MOVIES;
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-8048S070 MJPEG PLAYER INIT...");

  gfx->begin();
  gfx->fillScreen(BLACK);
  initBacklight();

  if (!initSDCard())
  {
    showStatus("INSERT SD CARD", "WAITING FOR MEDIA");
    return;
  }

  showStatus("PLAYER READY", "READING /mjpeg");
}

void loop()
{
  if (!ensureSDCardReady())
  {
    return;
  }

  MovieScanResult result = playMoviesFromDirectory(MJPEG_DIRECTORY_PATH);

  if (result == MOVIE_SCAN_NO_FOLDER)
  {
    showStatus("NO /mjpeg FOLDER", "CREATE FOLDER ON SD");
    delay(MJPEG_RETRY_DELAY_MS);
    return;
  }

  if (result == MOVIE_SCAN_NO_MOVIES)
  {
    showStatus("NO MOVIES TO PLAY", "COPY .mjpeg TO /mjpeg");
    delay(MJPEG_RETRY_DELAY_MS);
    return;
  }

  delay(10);
}
