// Purpose: Stream MP3 files from SD, decode with minimp3, play via I2S.
#pragma once

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "minimp3.h"
#include "I2sPcmOutput.h"
#include "app_config.h"

class Mp3Player
{
public:
  bool start(const char *path, SemaphoreHandle_t sdMutex)
  {
    stop();

    if (path == nullptr)
    {
      return false;
    }

    audioFile = SD.open(path, FILE_READ);
    if (!audioFile || audioFile.isDirectory())
    {
      Serial.print("MP3 open failed: ");
      Serial.println(path);
      return false;
    }

    decoder = (mp3dec_t *)heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    decodeScratch = (uint8_t *)heap_caps_malloc(MINIMP3_DECODE_SCRATCH_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    compressedBuf = (uint8_t *)heap_caps_malloc(MP3_COMPRESSED_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pcmBuf = (int16_t *)heap_caps_malloc(sizeof(int16_t) * MINIMP3_MAX_SAMPLES_PER_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (decoder == nullptr || decodeScratch == nullptr || compressedBuf == nullptr || pcmBuf == nullptr)
    {
      Serial.println("MP3 decoder/buffer alloc failed");
      stop();
      return false;
    }

    mp3dec_init(decoder);
    decoder->decode_scratch = decodeScratch;
    compressedLen = 0;
    compressedPos = 0;
    pcmBytesPlayed = 0;
    outputSampleRate = 0;
    sdCardMutex = sdMutex;
    stopRequested = false;
    eofReached = false;

    if (!I2sPcmOutput::configure(AUDIO_SAMPLE_RATE))
    {
      stop();
      return false;
    }

    playing = true;

    if (!I2sPcmOutput::enable())
    {
      stop();
      return false;
    }

    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCore(
            taskEntry, "mp3Player", MP3_PLAYER_TASK_STACK, this,
            AUDIO_PLAYER_TASK_PRIORITY, &task, AUDIO_PLAYER_TASK_CORE) != pdPASS)
    {
      stop();
      return false;
    }

    Serial.printf("Audio: %s (MP3 streaming)\n", path);
    return true;
  }

  void stop()
  {
    if (!playing && decoder == nullptr && decodeScratch == nullptr && compressedBuf == nullptr && !audioFile)
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

    if (decoder != nullptr)
    {
      decoder->decode_scratch = nullptr;
      heap_caps_free(decoder);
      decoder = nullptr;
    }
    if (decodeScratch != nullptr)
    {
      heap_caps_free(decodeScratch);
      decodeScratch = nullptr;
    }
    if (compressedBuf != nullptr)
    {
      heap_caps_free(compressedBuf);
      compressedBuf = nullptr;
    }
    if (pcmBuf != nullptr)
    {
      heap_caps_free(pcmBuf);
      pcmBuf = nullptr;
    }

    playing = false;
    stopRequested = false;
    compressedLen = 0;
    compressedPos = 0;
    pcmBytesPlayed = 0;
    outputSampleRate = 0;
    eofReached = false;
  }

  bool isPlaying() const
  {
    return playing;
  }

  uint32_t getElapsedMs() const
  {
    if (outputSampleRate == 0)
    {
      return 0;
    }
    return (uint32_t)((pcmBytesPlayed * 1000ULL) / (outputSampleRate * 2ULL));
  }

  uint32_t getBytesPlayed() const
  {
    return (uint32_t)pcmBytesPlayed;
  }

private:
  static void taskEntry(void *param)
  {
    Mp3Player *self = static_cast<Mp3Player *>(param);
    self->decodeLoop();
    self->playing = false;
    vTaskDelete(nullptr);
  }

  bool refillCompressed()
  {
    if (eofReached || compressedBuf == nullptr || !audioFile)
    {
      return false;
    }

    if (compressedPos > 0 && compressedPos < compressedLen)
    {
      memmove(compressedBuf, compressedBuf + compressedPos, compressedLen - compressedPos);
      compressedLen -= compressedPos;
      compressedPos = 0;
    }
    else if (compressedPos >= compressedLen)
    {
      compressedLen = 0;
      compressedPos = 0;
    }

    size_t space = MP3_COMPRESSED_BUF_BYTES - compressedLen;
    if (space == 0)
    {
      return true;
    }

    size_t loaded = 0;
    if (sdCardMutex != nullptr)
    {
      xSemaphoreTake(sdCardMutex, portMAX_DELAY);
    }
    while (loaded < space)
    {
      int n = audioFile.read(compressedBuf + compressedLen + loaded, space - loaded);
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

    if (loaded == 0)
    {
      eofReached = true;
      return compressedLen > compressedPos;
    }

    compressedLen += loaded;
    return true;
  }

  void decodeLoop()
  {
    mp3dec_frame_info_t info = {};

    while (!stopRequested)
    {
      if (compressedLen - compressedPos < 1024 && !eofReached)
      {
        if (!refillCompressed())
        {
          break;
        }
      }

      if (compressedLen <= compressedPos)
      {
        if (eofReached)
        {
          break;
        }
        vTaskDelay(1);
        continue;
      }

      int samples = mp3dec_decode_frame(
          decoder,
          compressedBuf + compressedPos,
          (int)(compressedLen - compressedPos),
          pcmBuf,
          &info);

      if (info.frame_bytes > 0)
      {
        compressedPos += (size_t)info.frame_bytes;
      }

      if (samples <= 0)
      {
        if (eofReached && compressedPos >= compressedLen)
        {
          break;
        }
        continue;
      }

      if (outputSampleRate == 0)
      {
        outputSampleRate = (uint32_t)info.hz;
        if (!I2sPcmOutput::configure(outputSampleRate))
        {
          break;
        }
        I2sPcmOutput::enable();
      }

      if (info.channels == 2)
      {
        for (int i = 0; i < samples; ++i)
        {
          pcmBuf[i] = pcmBuf[i * 2];
        }
      }

      const size_t monoBytes = (size_t)samples * 2u;
      size_t written = 0;
      if (!I2sPcmOutput::write((const uint8_t *)pcmBuf, monoBytes, &written))
      {
        break;
      }
      pcmBytesPlayed += written;
    }

    if (audioFile)
    {
      audioFile.close();
      audioFile = File();
    }
  }

  File audioFile;
  mp3dec_t *decoder = nullptr;
  uint8_t *decodeScratch = nullptr;
  uint8_t *compressedBuf = nullptr;
  int16_t *pcmBuf = nullptr;
  size_t compressedLen = 0;
  size_t compressedPos = 0;
  SemaphoreHandle_t sdCardMutex = nullptr;
  volatile bool playing = false;
  volatile bool stopRequested = false;
  volatile bool eofReached = false;
  volatile uint64_t pcmBytesPlayed = 0;
  uint32_t outputSampleRate = 0;
};
