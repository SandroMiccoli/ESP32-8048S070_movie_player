// Purpose: Shared I2S PCM output for the onboard NS4168 amplifier (Speak / P6).
#pragma once

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/i2s_std.h>

#include "app_config.h"

class I2sPcmOutput
{
public:
  // Drive BCLK/LRCLK/DOUT low so the NS4168 stays shut down (no speaker energy).
  // Call as early as possible on boot; also used after every stop.
  static void holdSilent()
  {
    tearDownChannel();

    const gpio_num_t pins[] = {
        (gpio_num_t)I2S_PIN_BCLK,
        (gpio_num_t)I2S_PIN_LRCLK,
        (gpio_num_t)I2S_PIN_DOUT,
    };

    for (gpio_num_t pin : pins)
    {
      gpio_reset_pin(pin);
      gpio_set_direction(pin, GPIO_MODE_OUTPUT);
      gpio_set_level(pin, 0);
      gpio_set_pull_mode(pin, GPIO_FLOATING);
    }
  }

  static bool configure(uint32_t sampleRate)
  {
    if (!ensureChannel())
    {
      return false;
    }

    if (configuredRate == sampleRate && channelReady)
    {
      return true;
    }

    i2s_channel_disable(txHandle);

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);

    if (i2s_channel_reconfig_std_clock(txHandle, &clk_cfg) != ESP_OK ||
        i2s_channel_reconfig_std_slot(txHandle, &slot_cfg) != ESP_OK)
    {
      Serial.println("I2S reconfig failed");
      return false;
    }

    configuredRate = sampleRate;
    channelReady = true;
    return true;
  }

  static bool enable()
  {
    if (!channelReady)
    {
      return false;
    }
    return i2s_channel_enable(txHandle) == ESP_OK;
  }

  static void disable()
  {
    // Fully release I2S and clamp amp pins low — no clocks, no speaker drive.
    holdSilent();
  }

  static bool write(const uint8_t *data, size_t len, size_t *written)
  {
    if (txHandle == nullptr || data == nullptr || len == 0)
    {
      return false;
    }

    const uint8_t vol = volumePercent;
    if (vol >= 100)
    {
      return i2s_channel_write(txHandle, data, len, written, portMAX_DELAY) == ESP_OK;
    }
    if (vol == 0)
    {
      // Keep stream timing with silence so A/V sync still advances.
      static uint8_t silence[512];
      size_t total = 0;
      while (total < len)
      {
        const size_t chunk = (len - total) > sizeof(silence) ? sizeof(silence) : (len - total);
        size_t w = 0;
        if (i2s_channel_write(txHandle, silence, chunk, &w, portMAX_DELAY) != ESP_OK)
        {
          if (written)
          {
            *written = total;
          }
          return false;
        }
        total += w;
      }
      if (written)
      {
        *written = total;
      }
      return true;
    }

    // Scale 16-bit mono PCM through a small stack buffer.
    int16_t scaled[128];
    size_t total = 0;
    const int16_t *src = reinterpret_cast<const int16_t *>(data);
    const size_t sampleCount = len / sizeof(int16_t);
    size_t samplePos = 0;
    while (samplePos < sampleCount)
    {
      const size_t n =
          (sampleCount - samplePos) > 128 ? 128 : (sampleCount - samplePos);
      for (size_t i = 0; i < n; ++i)
      {
        scaled[i] = static_cast<int16_t>((static_cast<int32_t>(src[samplePos + i]) * vol) / 100);
      }
      size_t w = 0;
      if (i2s_channel_write(txHandle, scaled, n * sizeof(int16_t), &w, portMAX_DELAY) != ESP_OK)
      {
        if (written)
        {
          *written = total;
        }
        return false;
      }
      total += w;
      samplePos += n;
    }
    if (written)
    {
      *written = total;
    }
    return true;
  }

  static void setVolumePercent(uint8_t percent)
  {
    if (percent > 100)
    {
      percent = 100;
    }
    volumePercent = percent;
  }

  static uint8_t getVolumePercent()
  {
    return volumePercent;
  }

  static bool ready()
  {
    return txHandle != nullptr;
  }

private:
  static void tearDownChannel()
  {
    if (txHandle == nullptr)
    {
      return;
    }

    i2s_channel_disable(txHandle);
    i2s_del_channel(txHandle);
    txHandle = nullptr;
    configuredRate = 0;
    channelReady = false;
  }

  static bool ensureChannel()
  {
    if (txHandle != nullptr)
    {
      return true;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 16;
    chan_cfg.dma_frame_num = 512;
    chan_cfg.auto_clear = true;

    if (i2s_new_channel(&chan_cfg, &txHandle, nullptr) != ESP_OK)
    {
      Serial.println("I2S channel alloc failed");
      return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_PIN_BCLK,
            .ws = (gpio_num_t)I2S_PIN_LRCLK,
            .dout = (gpio_num_t)I2S_PIN_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    if (i2s_channel_init_std_mode(txHandle, &std_cfg) != ESP_OK)
    {
      Serial.println("I2S init failed");
      i2s_del_channel(txHandle);
      txHandle = nullptr;
      return false;
    }

    configuredRate = AUDIO_SAMPLE_RATE;
    channelReady = true;
    Serial.println("I2S audio ready (NS4168 MSB format)");
    return true;
  }

  static i2s_chan_handle_t txHandle;
  static uint32_t configuredRate;
  static bool channelReady;
  static volatile uint8_t volumePercent;
};

i2s_chan_handle_t I2sPcmOutput::txHandle = nullptr;
uint32_t I2sPcmOutput::configuredRate = 0;
bool I2sPcmOutput::channelReady = false;
volatile uint8_t I2sPcmOutput::volumePercent = AUDIO_VOLUME_PERCENT_DEFAULT;
