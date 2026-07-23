#pragma once

// ---------------------------------------------------------------------------
// Display — ESP32-8048S070 7.0" 800x480 RGB565 DPI panel (ST7262)
// ---------------------------------------------------------------------------

#define LCD_WIDTH 800
#define LCD_HEIGHT 480

#define TFT_BL 2

// RGB data / sync pins (Profile B from board PDQgraphicstest demo)
#define LCD_PIN_DE 41
#define LCD_PIN_VSYNC 40
#define LCD_PIN_HSYNC 39
#define LCD_PIN_PCLK 42

#define LCD_PIN_R0 14
#define LCD_PIN_R1 21
#define LCD_PIN_R2 47
#define LCD_PIN_R3 48
#define LCD_PIN_R4 45

#define LCD_PIN_G0 9
#define LCD_PIN_G1 46
#define LCD_PIN_G2 3
#define LCD_PIN_G3 8
#define LCD_PIN_G4 16
#define LCD_PIN_G5 1

#define LCD_PIN_B0 15
#define LCD_PIN_B1 7
#define LCD_PIN_B2 6
#define LCD_PIN_B3 5
#define LCD_PIN_B4 4

// Panel timing — Profile B (16 MHz). See README for Profile A fallback.
#define LCD_HSYNC_POLARITY 0
#define LCD_HSYNC_FRONT_PORCH 210
#define LCD_HSYNC_PULSE_WIDTH 30
#define LCD_HSYNC_BACK_PORCH 16

#define LCD_VSYNC_POLARITY 0
#define LCD_VSYNC_FRONT_PORCH 22
#define LCD_VSYNC_PULSE_WIDTH 13
#define LCD_VSYNC_BACK_PORCH 10

// Profile A
// #define LCD_HSYNC_POLARITY 0
// #define LCD_HSYNC_FRONT_PORCH 8
// #define LCD_HSYNC_PULSE_WIDTH 2
// #define LCD_HSYNC_BACK_PORCH 43

// #define LCD_VSYNC_POLARITY 0
// #define LCD_VSYNC_FRONT_PORCH 8
// #define LCD_VSYNC_PULSE_WIDTH 2
// #define LCD_VSYNC_BACK_PORCH 12

#define LCD_PCLK_ACTIVE_NEG 1
#define LCD_PCLK_HZ 8000000

// Recommended when the framebuffer lives in PSRAM (ESP-IDF RGB LCD driver).
#define LCD_BOUNCE_BUFFER_PX (LCD_WIDTH * 10)

// ---------------------------------------------------------------------------
// SD card — SPI (board microSD slot)
// ---------------------------------------------------------------------------

#define SD_CS 10
#define SPI_MOSI 11
#define SPI_MISO 13
#define SPI_SCK 12
#define SD_SPI_FREQ_HZ 40000000
// Lower SPI clock while MJPEG + audio stream share the bus (reduces glitches/crashes).
#define SD_SPI_FREQ_AV_HZ 20000000

// ---------------------------------------------------------------------------
// I2S audio — onboard NS4168 (ESP32-8048S070 v1.4; v1.0 boards may need BCLK=19)
// ---------------------------------------------------------------------------

#define I2S_PIN_BCLK 0
#define I2S_PIN_LRCLK 18
#define I2S_PIN_DOUT 17

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_CHANNELS 1

// MP3 streaming buffer (compressed bytes read from SD — much smaller than PCM WAV).
#define MP3_COMPRESSED_BUF_BYTES 8192

// WAVs that fit in PSRAM are loaded once (no SD reads during playback — most reliable).
// 16 kHz mono 16-bit ≈ 32 KB/s → 5 MB holds ~2.6 minutes of audio.
#define AUDIO_MAX_PRELOAD_BYTES (5u * 1024u * 1024u)
#define AUDIO_STREAM_RING_BYTES (384u * 1024u)
#define AUDIO_SD_READ_BYTES 2048
#define AUDIO_WRITE_CHUNK_BYTES 1024
#define AUDIO_PLAYER_TASK_CORE 1
#define AUDIO_PLAYER_TASK_STACK 6144
#define MP3_PLAYER_TASK_STACK 8192
#define AUDIO_PLAYER_TASK_PRIORITY 10

// Encoded MJPEG frame rate used for A/V sync (must match ffmpeg fps= value).
#define MJPEG_FRAME_RATE 8

// Max wait when video decode runs ahead of the audio clock.
#define AV_SYNC_MAX_WAIT_MS 200

// ---------------------------------------------------------------------------
// MJPEG playback
// ---------------------------------------------------------------------------

#define MJPEG_DIRECTORY_PATH "/mjpeg"
#define MJPEG_BUFFER_SIZE_BYTES (512 * 1024)
#define MJPEG_USE_BIG_ENDIAN true
#define MJPEG_RETRY_DELAY_MS 3000

// Idle / alert paths for MQTT sound-trigger mode (must exist on the SD card).
#define MEDIA_IDLE_PATH "/mjpeg/idle.mjpeg"
#define MEDIA_ALERT_PATH "/mjpeg/alert.mjpeg"

// When true, loop idle.mjpeg and switch to alert.mjpeg on MQTT "alert".
// When false, keep the legacy sequential scan of every .mjpeg in /mjpeg.
#define MQTT_TRIGGER_MODE true

// ---------------------------------------------------------------------------
// WiFi STA + MQTT (RPi Access Point + Mosquitto)
// SSID/password must match rpi_sound_trigger/install/setup_ap.sh defaults.
// ---------------------------------------------------------------------------

#define WIFI_SSID "BOCAS-TELAS"
#define WIFI_PASSWORD "bocas123"
#define WIFI_COUNTRY_CODE "BR"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_RETRY_DELAY_MS 5000

#define MQTT_HOST "192.168.4.1"
#define MQTT_PORT 1883
#define MQTT_TOPIC "displays/trigger"
#define MQTT_CLIENT_ID_PREFIX "esp32-display-"
#define MQTT_TASK_STACK 6144
#define MQTT_TASK_PRIORITY 1
#define MQTT_TASK_CORE 0
#define MQTT_RECONNECT_DELAY_MS 3000
#define MQTT_KEEPALIVE_S 30

// ---------------------------------------------------------------------------
// Dual-core pipeline — SD reading/framing runs on a task pinned to one core
// while JPEG decode + draw runs on the loop core. Complete JPEG frames are
// copied into a small ring of PSRAM "slots" handed between the two cores.
// ---------------------------------------------------------------------------

#define MJPEG_FRAME_SLOT_COUNT 3
#define MJPEG_FRAME_SLOT_BYTES (192 * 1024) // max size of a single JPEG frame
#define MJPEG_READER_TASK_CORE 0
#define MJPEG_READER_TASK_STACK 8192
#define MJPEG_READER_TASK_PRIORITY 2
