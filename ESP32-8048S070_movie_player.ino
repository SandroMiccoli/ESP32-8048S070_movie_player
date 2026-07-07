// ESP32-8048S070 7.0" MJPEG movie player — autoplay loop from SPI SD card.
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <Arduino_GFX_Library.h>

#include "app_config.h"
#include "MjpegClass.h"
#include "WavPlayer.h"
#include "Mp3Player.h"

enum class AudioKind
{
  None,
  Wav,
  Mp3
};

static AudioKind activeAudioKind = AudioKind::None;

// ---------------------------------------------------------------------------
// Display (Arduino_GFX RGB panel — ESP32-8048S070, Arduino_GFX 3.x API)
// ---------------------------------------------------------------------------

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    LCD_PIN_DE, LCD_PIN_VSYNC, LCD_PIN_HSYNC, LCD_PIN_PCLK,
    LCD_PIN_R0, LCD_PIN_R1, LCD_PIN_R2, LCD_PIN_R3, LCD_PIN_R4,
    LCD_PIN_G0, LCD_PIN_G1, LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5,
    LCD_PIN_B0, LCD_PIN_B1, LCD_PIN_B2, LCD_PIN_B3, LCD_PIN_B4,
    LCD_HSYNC_POLARITY, LCD_HSYNC_FRONT_PORCH, LCD_HSYNC_PULSE_WIDTH, LCD_HSYNC_BACK_PORCH,
    LCD_VSYNC_POLARITY, LCD_VSYNC_FRONT_PORCH, LCD_VSYNC_PULSE_WIDTH, LCD_VSYNC_BACK_PORCH,
    LCD_PCLK_ACTIVE_NEG, LCD_PCLK_HZ, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */, LCD_BOUNCE_BUFFER_PX);

Arduino_GFX *gfx = new Arduino_RGB_Display(
    LCD_WIDTH, LCD_HEIGHT, rgbpanel, 0 /* rotation */, false /* auto_flush */);

// ---------------------------------------------------------------------------
// MJPEG playback state
// ---------------------------------------------------------------------------

static MjpegClass mjpeg;
static File mjpegFile;
static uint8_t *mjpegBuffer = nullptr;
static bool sdCardReady = false;
static SemaphoreHandle_t sdCardMutex = nullptr;

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
  ledcAttach(TFT_BL, 600, 8);
  ledcWrite(TFT_BL, 255);
}

static void showStatus(const char *line1, const char *line2 = nullptr)
{
  gfx->fillScreen(RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
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

// Companion audio: same basename as the MJPEG file (.mp3 preferred, .wav fallback).
static String basePathForMjpeg(const char *mjpegPath)
{
  String base = mjpegPath;
  if (pathEndsWith(mjpegPath, ".mjpeg"))
  {
    base.remove(base.length() - 6);
  }
  else if (pathEndsWith(mjpegPath, ".mjpg"))
  {
    base.remove(base.length() - 5);
  }
  return base;
}

static bool companionAudioExists(const char *path)
{
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory())
  {
    if (f)
    {
      f.close();
    }
    return false;
  }
  f.close();
  return true;
}

static void stopActiveAudio(WavPlayer &wavPlayer, Mp3Player &mp3Player)
{
  if (activeAudioKind == AudioKind::Wav)
  {
    wavPlayer.stop();
  }
  else if (activeAudioKind == AudioKind::Mp3)
  {
    mp3Player.stop();
  }
  activeAudioKind = AudioKind::None;
}

static bool audioIsPlaying(const WavPlayer &wavPlayer, const Mp3Player &mp3Player)
{
  if (activeAudioKind == AudioKind::Wav)
  {
    return wavPlayer.isPlaying();
  }
  if (activeAudioKind == AudioKind::Mp3)
  {
    return mp3Player.isPlaying();
  }
  return false;
}

static uint32_t audioElapsedMs(const WavPlayer &wavPlayer, const Mp3Player &mp3Player)
{
  if (activeAudioKind == AudioKind::Wav)
  {
    return wavPlayer.getElapsedMs();
  }
  if (activeAudioKind == AudioKind::Mp3)
  {
    return mp3Player.getElapsedMs();
  }
  return 0;
}

static uint32_t audioBytesPlayed(const WavPlayer &wavPlayer, const Mp3Player &mp3Player)
{
  if (activeAudioKind == AudioKind::Wav)
  {
    return wavPlayer.getBytesPlayed();
  }
  if (activeAudioKind == AudioKind::Mp3)
  {
    return mp3Player.getBytesPlayed();
  }
  return 0;
}

static bool startCompanionAudio(
    const char *mjpegPath,
    WavPlayer &wavPlayer,
    Mp3Player &mp3Player,
    SemaphoreHandle_t sdMutex)
{
  stopActiveAudio(wavPlayer, mp3Player);

  const String base = basePathForMjpeg(mjpegPath);
  const String mp3Path = base + ".mp3";
  const String wavPath = base + ".wav";

  Serial.print("MJPEG: ");
  Serial.println(mjpegPath);
  Serial.print("Audio: ");
  Serial.flush();

  if (companionAudioExists(mp3Path.c_str()))
  {
    Serial.println(mp3Path);
    Serial.flush();
    if (mp3Player.start(mp3Path.c_str(), sdMutex))
    {
      activeAudioKind = AudioKind::Mp3;
      return true;
    }
    Serial.println("MP3 start failed");
  }
  else if (companionAudioExists(wavPath.c_str()))
  {
    Serial.println(wavPath);
    Serial.flush();
    if (wavPlayer.start(wavPath.c_str(), sdMutex))
    {
      activeAudioKind = AudioKind::Wav;
      return true;
    }
    Serial.println("WAV start failed");
  }
  else
  {
    Serial.println("(not found — expected .mp3 or .wav)");
  }

  Serial.flush();
  activeAudioKind = AudioKind::None;
  return false;
}

static void waitForAvSync(const WavPlayer &wavPlayer, const Mp3Player &mp3Player, uint32_t frameIndex)
{
  const uint32_t targetMs = (frameIndex * 1000u) / MJPEG_FRAME_RATE;
  const uint32_t deadlineMs = millis() + 500;
  while (audioIsPlaying(wavPlayer, mp3Player))
  {
    if (audioElapsedMs(wavPlayer, mp3Player) >= targetMs)
    {
      return;
    }
    if ((int32_t)(millis() - deadlineMs) >= 0)
    {
      Serial.printf("AV sync timeout at frame %u (audio=%u ms)\n",
                    (unsigned)frameIndex, audioElapsedMs(wavPlayer, mp3Player));
      return;
    }
    vTaskDelay(1);
  }
}

static void printResetReason()
{
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print("Reset reason: ");
  switch (reason)
  {
  case ESP_RST_POWERON:
    Serial.println("power-on");
    break;
  case ESP_RST_SW:
    Serial.println("software");
    break;
  case ESP_RST_PANIC:
    Serial.println("panic/crash (often PSRAM/framebuffer — see message below if repeated)");
    break;
  case ESP_RST_INT_WDT:
    Serial.println("interrupt watchdog");
    break;
  case ESP_RST_TASK_WDT:
    Serial.println("task watchdog");
    break;
  case ESP_RST_WDT:
    Serial.println("other watchdog");
    break;
  case ESP_RST_BROWNOUT:
    Serial.println("brownout");
    break;
  default:
    Serial.println((int)reason);
    break;
  }
}

// RGB panel needs ~750 KB contiguous PSRAM for an 800x480 framebuffer.
static bool verifyPsramForDisplay()
{
  constexpr uint32_t kFramebufferBytes = (uint32_t)LCD_WIDTH * (uint32_t)LCD_HEIGHT * 2u;

  Serial.printf("Flash %u MB, PSRAM %u MB, free PSRAM %u KB (LCD needs %u KB)\n",
                ESP.getFlashChipSize() / (1024u * 1024u),
                ESP.getPsramSize() / (1024u * 1024u),
                ESP.getFreePsram() / 1024u,
                kFramebufferBytes / 1024u);

  if (ESP.getPsramSize() == 0)
  {
    Serial.println();
    Serial.println("FATAL: PSRAM not detected — display cannot start.");
    Serial.println("Arduino IDE -> Tools, set ALL of the following:");
    Serial.println("  Board: ESP32S3 Dev Module");
    Serial.println("  PSRAM: OPI PSRAM          <-- required");
    Serial.println("  Flash Size: 16MB (128Mb)");
    Serial.println("  Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)");
    Serial.println("  Flash Mode: QIO");
    Serial.println("Then upload again.");
    return false;
  }

  void *probe = heap_caps_aligned_alloc(64, kFramebufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (probe == nullptr)
  {
    Serial.println();
    Serial.println("FATAL: PSRAM present but RGB framebuffer alloc failed.");
    Serial.println("Try PSRAM: OPI PSRAM (not QSPI). Reboot after changing Tools menu.");
    return false;
  }
  heap_caps_free(probe);
  return true;
}

static void haltWithStatus(const char *line1, const char *line2 = nullptr)
{
  Serial.println(line1);
  if (line2 != nullptr)
  {
    Serial.println(line2);
  }
  while (true)
  {
    delay(1000);
  }
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
  if (sdCardMutex == nullptr)
  {
    sdCardMutex = xSemaphoreCreateMutex();
  }
  Serial.println("SD card mounted");
  return true;
}

static void setSdSpiFrequency(uint32_t hz)
{
  SPI.setFrequency(hz);
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
    bool gotFrame = false;
    if (sdCardMutex != nullptr)
    {
      xSemaphoreTake(sdCardMutex, portMAX_DELAY);
    }
    gotFrame = mjpeg.readMjpegBuf();
    if (sdCardMutex != nullptr)
    {
      xSemaphoreGive(sdCardMutex);
    }
    if (!gotFrame)
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

  WavPlayer wavPlayer;
  Mp3Player mp3Player;

  mjpegFile = SD.open(path, FILE_READ);
  if (!mjpegFile || mjpegFile.isDirectory())
  {
    Serial.print("mjpeg open failed: ");
    Serial.println(path);
    stopActiveAudio(wavPlayer, mp3Player);
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
    stopActiveAudio(wavPlayer, mp3Player);
    return false;
  }

  gfx->fillScreen(RGB565_BLACK);
  gfx->flush();

  // Reset the slot ring: every slot starts free, no frames pending.
  xQueueReset(freeSlotQueue);
  xQueueReset(filledSlotQueue);
  for (int i = 0; i < MJPEG_FRAME_SLOT_COUNT; ++i)
  {
    xQueueSend(freeSlotQueue, &i, 0);
  }

  if (!startCompanionAudio(path, wavPlayer, mp3Player, sdCardMutex))
  {
    Serial.println("Playing video only (no audio)");
    Serial.flush();
  }
  else
  {
    Serial.println("Audio playback active");
    Serial.flush();
  }

  if (audioIsPlaying(wavPlayer, mp3Player))
  {
    setSdSpiFrequency(SD_SPI_FREQ_AV_HZ);
  }

  TaskHandle_t readerTask = nullptr;
  if (xTaskCreatePinnedToCore(
          mjpegReaderTask, "mjpegReader", MJPEG_READER_TASK_STACK, nullptr,
          MJPEG_READER_TASK_PRIORITY, &readerTask, MJPEG_READER_TASK_CORE) != pdPASS)
  {
    Serial.println("reader task create failed");
    mjpegFile.close();
    stopActiveAudio(wavPlayer, mp3Player);
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

    if (audioIsPlaying(wavPlayer, mp3Player))
    {
      waitForAvSync(wavPlayer, mp3Player, frameCount);
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
  const uint32_t audioMs = audioElapsedMs(wavPlayer, mp3Player);
  const uint32_t audioBytes = audioBytesPlayed(wavPlayer, mp3Player);
  stopActiveAudio(wavPlayer, mp3Player);
  setSdSpiFrequency(SD_SPI_FREQ_HZ);

  Serial.printf("Audio done: %u bytes, %u ms\n", audioBytes, audioMs);
  Serial.flush();

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
  delay(500);
  Serial.println("ESP32-8048S070 MJPEG PLAYER INIT...");
  printResetReason();

  if (!verifyPsramForDisplay())
  {
    haltWithStatus("PSRAM REQUIRED", "SEE SERIAL LOG");
  }

  if (!gfx->begin())
  {
    haltWithStatus("DISPLAY INIT FAILED", "SEE SERIAL LOG");
  }

  gfx->fillScreen(RGB565_BLACK);
  initBacklight();

  if (!initSDCard())
  {
    showStatus("INSERT SD CARD", "WAITING FOR MEDIA");
    return;
  }

  showStatus("PLAYER READY", "READING /mjpeg");
  Serial.println("Setup complete");
  Serial.flush();
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
