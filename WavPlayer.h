// Purpose: Stream 16-bit PCM WAV files to the onboard NS4168 I2S amplifier.
#pragma once

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "I2sPcmOutput.h"
#include "app_config.h"

class WavPlayer
{
public:
  static bool begin()
  {
    return I2sPcmOutput::configure(AUDIO_SAMPLE_RATE);
  }

  bool start(const char *path, SemaphoreHandle_t sdMutex)
  {
    stop();

    if (!begin() || path == nullptr)
    {
      return false;
    }

    audioFile = SD.open(path, FILE_READ);
    if (!audioFile || audioFile.isDirectory())
    {
      Serial.print("WAV open failed: ");
      Serial.println(path);
      return false;
    }

    WavHeaderInfo info;
    if (!parseWavHeaderInfo(audioFile, info))
    {
      Serial.print("WAV header invalid: ");
      Serial.println(path);
      audioFile.close();
      audioFile = File();
      return false;
    }

    if (info.channels != AUDIO_CHANNELS || info.bitsPerSample != AUDIO_BITS_PER_SAMPLE)
    {
      Serial.printf(
          "WAV format mismatch (%u ch, %u-bit); expected mono %u-bit\n",
          info.channels, info.bitsPerSample, AUDIO_BITS_PER_SAMPLE);
      audioFile.close();
      audioFile = File();
      return false;
    }

    if (info.sampleRate != 8000 && info.sampleRate != 16000)
    {
      Serial.printf(
          "WAV sample rate %u Hz not supported (use 8000 or 16000)\n",
          info.sampleRate);
      audioFile.close();
      audioFile = File();
      return false;
    }

    if (info.dataSize == 0)
    {
      Serial.println("WAV has no PCM data");
      audioFile.close();
      audioFile = File();
      return false;
    }

    if (!I2sPcmOutput::configure(info.sampleRate))
    {
      audioFile.close();
      audioFile = File();
      return false;
    }

    sdCardMutex = sdMutex;
    bytesPlayed = 0;
    bytesPerSecond = (uint32_t)info.sampleRate * info.channels * (info.bitsPerSample / 8u);
    stopRequested = false;

    if (!allocTaskIoBuffer())
    {
      audioFile.close();
      audioFile = File();
      return false;
    }

    const bool usePreload = info.dataSize <= AUDIO_MAX_PRELOAD_BYTES;
    if (usePreload)
    {
      if (!loadPreloadedPcm(audioFile, info))
      {
        audioFile.close();
        audioFile = File();
        return false;
      }
      audioFile.close();
      audioFile = File();
      mode = PlayMode::Preloaded;
      Serial.printf("Audio: %s (%u bytes WAV preloaded)\n", path, (unsigned)pcmLen);
    }
    else
    {
      if (!startStreaming(info))
      {
        audioFile.close();
        audioFile = File();
        return false;
      }
      Serial.printf("Audio: %s (%lu bytes WAV streaming — prefer MP3, see README)\n",
                    path, (unsigned long)info.dataSize);
    }

    playing = true;

    if (!I2sPcmOutput::enable())
    {
      stop();
      return false;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(
            audioTaskEntry, "wavPlayer", AUDIO_PLAYER_TASK_STACK, this,
            AUDIO_PLAYER_TASK_PRIORITY, &task, AUDIO_PLAYER_TASK_CORE) != pdPASS)
    {
      stop();
      return false;
    }

    return true;
  }

  void stop()
  {
    if (!playing && pcmBuffer == nullptr && ringBuffer == nullptr && taskIoBuffer == nullptr && !audioFile)
    {
      return;
    }

    stopRequested = true;
    for (int i = 0; i < 200 && playing; ++i)
    {
      vTaskDelay(pdMS_TO_TICKS(10));
    }

    I2sPcmOutput::disable();

    if (audioFile)
    {
      audioFile.close();
      audioFile = File();
    }

    freePcmBuffer();
    freeRingBuffer();
    freeTaskIoBuffer();
    mode = PlayMode::None;
    playing = false;
    stopRequested = false;
    pcmPos = 0;
    bytesPlayed = 0;
  }

  bool isPlaying() const
  {
    return playing;
  }

  uint32_t getElapsedMs() const
  {
    if (bytesPerSecond == 0)
    {
      return 0;
    }
    return (uint32_t)((bytesPlayed * 1000ULL) / bytesPerSecond);
  }

  uint32_t getBytesPlayed() const
  {
    return (uint32_t)bytesPlayed;
  }

private:
  enum class PlayMode
  {
    None,
    Preloaded,
    Streaming
  };

  struct WavHeaderInfo
  {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataOffset = 0;
    uint32_t dataSize = 0;
  };

  static void audioTaskEntry(void *param)
  {
    WavPlayer *self = static_cast<WavPlayer *>(param);
    if (self->mode == PlayMode::Preloaded)
    {
      self->audioTaskLoopPreloaded();
    }
    else
    {
      self->audioTaskLoopStreaming();
    }
    self->playing = false;
    vTaskDelete(nullptr);
  }

  bool loadPreloadedPcm(File &wavFile, const WavHeaderInfo &info)
  {
    pcmBuffer = (uint8_t *)heap_caps_malloc(info.dataSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcmBuffer == nullptr)
    {
      Serial.println("WAV preload alloc failed");
      return false;
    }

    wavFile.seek(info.dataOffset);
    size_t loaded = readFileBytes(wavFile, pcmBuffer, info.dataSize);
    if (loaded != info.dataSize)
    {
      Serial.printf("WAV preload short read (%u/%u)\n", (unsigned)loaded, info.dataSize);
      freePcmBuffer();
      return false;
    }

    pcmLen = loaded;
    pcmPos = 0;
    return true;
  }

  bool startStreaming(const WavHeaderInfo &info)
  {
    ringBuffer = (uint8_t *)heap_caps_malloc(AUDIO_STREAM_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ringBuffer == nullptr)
    {
      Serial.println("Audio ring buffer alloc failed");
      return false;
    }

    ringCapacity = AUDIO_STREAM_RING_BYTES;
    ringFill = 0;
    ringReadPos = 0;
    ringWritePos = 0;
    streamBytesRemaining = info.dataSize;

    audioFile.seek(info.dataOffset);
    mode = PlayMode::Streaming;

    refillRingBuffer();
    return true;
  }

  bool allocTaskIoBuffer()
  {
    if (taskIoBuffer != nullptr)
    {
      return true;
    }

    taskIoBuffer = (uint8_t *)heap_caps_malloc(AUDIO_SD_READ_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (taskIoBuffer == nullptr)
    {
      Serial.println("Audio task I/O buffer alloc failed");
      return false;
    }
    return true;
  }

  void freeTaskIoBuffer()
  {
    if (taskIoBuffer != nullptr)
    {
      heap_caps_free(taskIoBuffer);
      taskIoBuffer = nullptr;
    }
  }

  size_t readFileBytes(File &file, uint8_t *dest, size_t len)
  {
    size_t loaded = 0;
    if (sdCardMutex != nullptr)
    {
      xSemaphoreTake(sdCardMutex, portMAX_DELAY);
    }
    while (loaded < len)
    {
      int n = file.read(dest + loaded, len - loaded);
      if (n <= 0)
      {
        break;
      }
      loaded += (size_t)n;
    }
    if (sdCardMutex != nullptr)
    {
      xSemaphoreGive(sdCardMutex);
    }
    return loaded;
  }

  size_t ringSpace() const
  {
    return ringCapacity - ringFill;
  }

  bool ringPush(const uint8_t *data, size_t len)
  {
    if (len > ringSpace())
    {
      return false;
    }

    size_t first = ringCapacity - ringWritePos;
    if (len <= first)
    {
      memcpy(ringBuffer + ringWritePos, data, len);
    }
    else
    {
      memcpy(ringBuffer + ringWritePos, data, first);
      memcpy(ringBuffer, data + first, len - first);
    }

    ringWritePos = (ringWritePos + len) % ringCapacity;
    ringFill += len;
    return true;
  }

  size_t ringPop(uint8_t *dest, size_t maxLen)
  {
    if (ringFill == 0 || maxLen == 0)
    {
      return 0;
    }

    size_t len = (maxLen < ringFill) ? maxLen : ringFill;
    size_t first = ringCapacity - ringReadPos;
    if (len <= first)
    {
      memcpy(dest, ringBuffer + ringReadPos, len);
    }
    else
    {
      memcpy(dest, ringBuffer + ringReadPos, first);
      memcpy(dest + first, ringBuffer, len - first);
    }

    ringReadPos = (ringReadPos + len) % ringCapacity;
    ringFill -= len;
    return len;
  }

  void refillRingBuffer()
  {
    if (taskIoBuffer == nullptr)
    {
      return;
    }

    while (streamBytesRemaining > 0 && ringSpace() > 0)
    {
      size_t want = ringSpace();
      if (want > AUDIO_SD_READ_BYTES)
      {
        want = AUDIO_SD_READ_BYTES;
      }
      if (want > streamBytesRemaining)
      {
        want = streamBytesRemaining;
      }

      size_t loaded = readFileBytes(audioFile, taskIoBuffer, want);
      if (loaded == 0)
      {
        break;
      }

      ringPush(taskIoBuffer, loaded);
      streamBytesRemaining -= loaded;
    }
  }

  void audioTaskLoopPreloaded()
  {
    if (taskIoBuffer == nullptr)
    {
      return;
    }

    while (!stopRequested && pcmBuffer != nullptr && pcmPos < pcmLen)
    {
      size_t remaining = pcmLen - pcmPos;
      size_t chunk = (remaining > AUDIO_WRITE_CHUNK_BYTES) ? AUDIO_WRITE_CHUNK_BYTES : remaining;
      memcpy(taskIoBuffer, pcmBuffer + pcmPos, chunk);

      size_t written = 0;
      if (!I2sPcmOutput::write(taskIoBuffer, chunk, &written))
      {
        break;
      }

      pcmPos += written;
      bytesPlayed += written;
    }
  }

  void audioTaskLoopStreaming()
  {
    if (taskIoBuffer == nullptr)
    {
      return;
    }

    while (!stopRequested)
    {
      if (ringFill < AUDIO_WRITE_CHUNK_BYTES * 2)
      {
        refillRingBuffer();
      }

      size_t chunk = ringPop(taskIoBuffer, AUDIO_WRITE_CHUNK_BYTES);
      if (chunk == 0)
      {
        if (streamBytesRemaining == 0)
        {
          break;
        }
        vTaskDelay(1);
        continue;
      }

      size_t written = 0;
      if (!I2sPcmOutput::write(taskIoBuffer, chunk, &written))
      {
        break;
      }

      bytesPlayed += written;
    }

    if (audioFile)
    {
      audioFile.close();
      audioFile = File();
    }
  }

  void freePcmBuffer()
  {
    if (pcmBuffer != nullptr)
    {
      heap_caps_free(pcmBuffer);
      pcmBuffer = nullptr;
    }
    pcmLen = 0;
  }

  void freeRingBuffer()
  {
    if (ringBuffer != nullptr)
    {
      heap_caps_free(ringBuffer);
      ringBuffer = nullptr;
    }
    ringCapacity = 0;
  }

  static bool readExact(File &file, uint8_t *dest, size_t len)
  {
    size_t total = 0;
    while (total < len)
    {
      int n = file.read(dest + total, len - total);
      if (n <= 0)
      {
        return false;
      }
      total += (size_t)n;
    }
    return true;
  }

  static uint16_t readLe16(const uint8_t *p)
  {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
  }

  static uint32_t readLe32(const uint8_t *p)
  {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }

  static bool parseWavHeaderInfo(File &file, WavHeaderInfo &info)
  {
    file.seek(0);
    uint8_t riff[12];
    if (!readExact(file, riff, sizeof(riff)))
    {
      return false;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0)
    {
      return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    while (file.available())
    {
      uint8_t chunkHdr[8];
      if (!readExact(file, chunkHdr, sizeof(chunkHdr)))
      {
        break;
      }

      uint32_t chunkSize = readLe32(chunkHdr + 4);
      if (memcmp(chunkHdr, "fmt ", 4) == 0)
      {
        if (chunkSize < 16)
        {
          return false;
        }
        uint8_t fmt[16];
        if (!readExact(file, fmt, sizeof(fmt)))
        {
          return false;
        }
        if (readLe16(fmt + 0) != 1)
        {
          return false;
        }
        info.channels = readLe16(fmt + 2);
        info.sampleRate = readLe32(fmt + 4);
        info.bitsPerSample = readLe16(fmt + 14);
        haveFmt = true;
        if (chunkSize > sizeof(fmt))
        {
          file.seek(file.position() + chunkSize - sizeof(fmt));
        }
      }
      else if (memcmp(chunkHdr, "data", 4) == 0)
      {
        info.dataOffset = (uint32_t)file.position();
        info.dataSize = chunkSize;
        haveData = true;
        break;
      }
      else
      {
        file.seek(file.position() + chunkSize);
      }
    }

    return haveFmt && haveData;
  }

  PlayMode mode = PlayMode::None;
  File audioFile;
  SemaphoreHandle_t sdCardMutex = nullptr;
  uint8_t *pcmBuffer = nullptr;
  size_t pcmLen = 0;
  size_t pcmPos = 0;
  uint8_t *ringBuffer = nullptr;
  uint8_t *taskIoBuffer = nullptr;
  size_t ringCapacity = 0;
  size_t ringReadPos = 0;
  size_t ringWritePos = 0;
  size_t ringFill = 0;
  size_t streamBytesRemaining = 0;
  volatile bool playing = false;
  volatile bool stopRequested = false;
  volatile uint64_t bytesPlayed = 0;
  uint32_t bytesPerSecond = 0;
};
