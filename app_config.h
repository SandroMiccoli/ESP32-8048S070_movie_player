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

#define LCD_PCLK_ACTIVE_NEG 1
#define LCD_PCLK_HZ 12000000

// ---------------------------------------------------------------------------
// SD card — SPI (board microSD slot)
// ---------------------------------------------------------------------------

#define SD_CS 10
#define SPI_MOSI 11
#define SPI_MISO 13
#define SPI_SCK 12
#define SD_SPI_FREQ_HZ 40000000

// ---------------------------------------------------------------------------
// MJPEG playback
// ---------------------------------------------------------------------------

#define MJPEG_DIRECTORY_PATH "/mjpeg"
#define MJPEG_BUFFER_SIZE_BYTES (512 * 1024)
#define MJPEG_USE_BIG_ENDIAN true
#define MJPEG_RETRY_DELAY_MS 3000

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
