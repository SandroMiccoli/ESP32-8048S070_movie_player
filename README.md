# ESP32-8048S070 Movie Player

Autoplay MJPEG videos from the microSD card on the Sunton/Jingcai **ESP32-8048S070** 7.0" board (800×480 RGB panel, ESP32-S3, 8 MB PSRAM). Optional synced **MP3** or **WAV** audio via the onboard NS4168 amplifier.

## Hardware

- **Board:** ESP32-8048S070 (ESP32-S3, 16 MB flash, OPI PSRAM)
- **Display:** 800×480 RGB565 parallel DPI panel (ST7262)
- **Storage:** Onboard microSD via SPI (CS=10, MOSI=11, MISO=13, SCK=12)

## Arduino IDE setup

1. Install the **ESP32** board package (3.x) from Espressif.
2. Select board: **ESP32S3 Dev Module** (or equivalent).
3. Enable **PSRAM: OPI PSRAM**.
4. Set **Flash Size: 16MB** and **Partition Scheme: Huge APP**.
5. Install libraries:
  - [JPEGDEC](https://github.com/bitbank2/JPEGDEC) (1.8.4+)
  - [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX) — copy from `7.0inch_ESP32-8048S070/1-Demo/Demo_Arduino/Libraries/Arduino_GFX-master` or install from Library Manager
6. Open `ESP32-8048S070_movie_player.ino` and upload.

**Important:** All sketch files must be in the same folder. Arduino IDE requires this layout:

```
ESP32-8048S070_movie_player/
  ESP32-8048S070_movie_player.ino
  app_config.h
  MjpegClass.h
  I2sPcmOutput.h
  WavPlayer.h
  Mp3Player.h
  minimp3.h
  minimp3.cpp
```

Open the `.ino` file directly — do not nest it in a subfolder.

### Arduino CLI

```bash
arduino-cli compile --profile esp32_8048s070 ESP32-8048S070_movie_player
arduino-cli upload --profile esp32_8048s070 -p COMx ESP32-8048S070_movie_player
```

## SD card layout

Create a folder named `mjpeg` at the root of the SD card and copy your video files there:

```
/mjpeg
  movie1.mjpeg
  movie1.mp3      ← optional synced audio (preferred)
  movie2.mjpg
  movie2.wav      ← WAV fallback
```

Supported extensions: `.mjpeg`, `.mjpg` (case-insensitive). Audio: same basename with `.mp3` (preferred) or `.wav`.

The player scans `/mjpeg`, plays every video once, then loops forever.

## Video conversion

Videos must be **MJPEG** (a stream of JPEG frames). Convert with ffmpeg.

Playback speed is dominated by the **software JPEG decode**, which scales roughly with pixel count, so the encoded resolution is the main lever. Pick one of these presets depending on whether you want quality or smoothness (measured frame rates on this board, after the dual-core pipeline below):


| Preset                     | ffmpeg `scale` | Approx. fps |
| -------------------------- | -------------- | ----------- |
| Full quality               | `800:480`      | ~10–11 fps  |
| **Balanced (recommended)** | `640:384`      | ~14–15 fps  |
| Smoothest                  | `480:288`      | ~24–25 fps  |


```bash
# Full quality — native panel resolution
ffmpeg -i input.mp4 -vf "scale=800:480:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output.mjpeg

# Balanced — recommended quality/smoothness trade-off
ffmpeg -i input.mp4 -vf "scale=640:384:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output_640.mjpeg

# Smoothest — highest frame rate, upscaled (slightly soft) on the 800x480 panel
ffmpeg -i input.mp4 -vf "scale=480:288:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output_480.mjpeg
```

Lower-resolution frames are centered on the panel (letterboxed at native size — no hardware upscaling), so they appear smaller/softer but decode much faster.

### Audio extraction (synced to 12 fps MJPEG)

The ESP32-8048S070 has an onboard **NS4168 I2S amplifier**.

**Recommended: MP3** — much smaller than WAV, low SD bandwidth (stable with video). The player decodes MP3 on the fly with [minimp3](https://github.com/lieff/minimp3).

**Optional: WAV** — 16-bit PCM mono at **16 kHz** or **8 kHz** (smaller). Large WAV files (>5 MB) stream from SD and may crash; prefer MP3 for long clips.

Export MJPEG and audio in **one ffmpeg pass** with **`-shortest`**:

```bash
# Recommended — 12 fps MJPEG + compact MP3
ffmpeg -i input.mp4 -filter_complex "[0:v]scale=640:384:force_original_aspect_ratio=decrease,fps=12[v]" -map "[v]" -q:v 9 -an output.mjpeg -map 0:a? -c:a libmp3lame -ar 16000 -ac 1 -b:a 32k -shortest output.mp3
```

Copy to the SD card as a matching pair:

```
/mjpeg
  output.mjpeg
  output.mp3
```

The player prefers `.mp3` over `.wav` when both exist.

**Size guide** (4.5 min clip):

| Format | Approx. size | Notes |
| ------ | ------------ | ----- |
| WAV 16 kHz | ~8.7 MB | SD streaming — avoid |
| WAV 8 kHz | ~4.3 MB | Supported if you set matching `-ar 8000` |
| **MP3 32 kbps** | **~1 MB** | **Recommended** |

WAV fallback (short clips only):

```bash
ffmpeg -i input.mp4 -filter_complex "[0:v]scale=640:384:force_original_aspect_ratio=decrease,fps=12[v]" -map "[v]" -q:v 9 -an output.mjpeg -map 0:a? -c:a pcm_s16le -ar 16000 -ac 1 -shortest output.wav
```

`0:a?` skips audio when the source has no audio track. `-shortest` keeps audio the same length as the video.

Tips for smoother playback:

- Keep resolution at or below **800×480**.
- Use **12–15 fps** for a good balance of quality and performance.
- Lower `-q:v` (e.g. 11–13) if frames are too large or SD reads stutter (shrinks files).
- You can also use the [Video Conversion Studio](https://thelastoutpostworkshop.github.io/video_conversion/) web tool.

## Performance notes

The player overlaps work across both ESP32-S3 cores: a reader task (pinned to core 0) pulls complete JPEG frames off the SD card into a small ring of PSRAM buffers, while the loop core decodes and draws them. SD read time is therefore hidden behind decode time. Per-file serial output reports `wait` / `decode+draw` / `flush` milliseconds — `wait` near zero means decode is the bottleneck (expected); a large `wait` means the SD reader can't keep up (try a lower `-q:v`).

Pipeline tuning lives in `app_config.h` (`MJPEG_FRAME_SLOT_COUNT`, `MJPEG_FRAME_SLOT_BYTES`, and the `MJPEG_READER_TASK_`* settings).

## Display flicker troubleshooting

The default timing uses **Profile B** at **16 MHz** (same as the board's PDQgraphics and LVGL demos). If the panel flickers:

1. In `app_config.h`, lower `LCD_PCLK_HZ` to **12000000** (12 MHz).
2. Or switch to **Profile A** timing (from `HelloWorld.ino`):

```c
#define LCD_HSYNC_FRONT_PORCH 8
#define LCD_HSYNC_PULSE_WIDTH 2
#define LCD_HSYNC_BACK_PORCH 43
#define LCD_VSYNC_FRONT_PORCH 8
#define LCD_VSYNC_PULSE_WIDTH 2
#define LCD_VSYNC_BACK_PORCH 12
```

## Configuration

Edit `app_config.h` to change:


| Setting      | Default            |
| ------------ | ------------------ |
| Video folder | `/mjpeg`           |
| MJPEG buffer | 512 KB (PSRAM)     |
| Frame slots  | 3 × 192 KB (PSRAM) |
| SD SPI clock | 40 MHz             |
| Panel PCLK   | 12 MHz             |


## On-screen messages


| Message           | Meaning                                         |
| ----------------- | ----------------------------------------------- |
| INSERT SD CARD    | No SD card detected                             |
| NO /mjpeg FOLDER  | Create `/mjpeg` on the SD card                  |
| NO MOVIES TO PLAY | Folder exists but has no `.mjpeg`/`.mjpg` files |


Serial output (115200 baud) logs playback stats per file.

## License

MIT — MJPEG parser adapted from the [ST7701 movie player](../ESP32-S3_3_16_ST7701_movie_player) project.