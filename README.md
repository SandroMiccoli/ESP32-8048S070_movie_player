# ESP32-8048S070 Movie Player

MJPEG player for the Sunton/Jingcai **ESP32-8048S070** 7.0" board (800×480 RGB panel, ESP32-S3, OPI PSRAM). Optional synced **MP3** or **WAV** via the onboard NS4168 amplifier.

By default the board joins a Raspberry Pi WiFi AP, subscribes to MQTT, loops **`idle.mjpeg`**, and switches to **`alert.mjpeg`** when the Pi mic app publishes a trigger. See [`rpi_sound_trigger/`](rpi_sound_trigger/) for the Pi side.

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
  - [PubSubClient](https://github.com/knolleary/pubsubclient) (Nick O'Leary) — MQTT for RPi sound-trigger mode
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
  MqttCommand.h
  MqttWifi.h
  minimp3.h
  minimp3.cpp
  rpi_sound_trigger/          ← Raspberry Pi app (separate; not compiled by Arduino)
```

Open the `.ino` file directly — do not nest it in a subfolder.

### Arduino CLI

```bash
arduino-cli compile --profile esp32_8048s070 ESP32-8048S070_movie_player
arduino-cli upload --profile esp32_8048s070 -p COMx ESP32-8048S070_movie_player
```

## SD card layout

With **MQTT trigger mode** enabled (`MQTT_TRIGGER_MODE` in `app_config.h`, default **on**), put two fixed clips on the card:

```
/mjpeg
  idle.mjpeg     ← loops while waiting
  idle.mp3       ← optional synced audio
  alert.mjpeg    ← plays once when RPi publishes alert
  alert.mp3      ← optional
```

On MQTT `{"state":"alert"}` every board aborts idle mid-play, plays `alert.mjpeg` once, then returns to looping idle. See [`rpi_sound_trigger/README.md`](rpi_sound_trigger/README.md) for the Pi Access Point, Mosquitto, and mic app.

To restore the old behaviour (scan and play every `.mjpeg` in order), set `#define MQTT_TRIGGER_MODE false`.

Legacy sequential layout (only when MQTT mode is off):

```
/mjpeg
  movie1.mjpeg
  movie1.mp3      ← optional synced audio (preferred)
  movie2.mjpg
  movie2.wav      ← WAV fallback
```

Supported extensions: `.mjpeg`, `.mjpg` (case-insensitive). Audio: same basename with `.mp3` (preferred) or `.wav`.

## WiFi + MQTT (multi-display)

Defaults in `app_config.h` (must match the Pi AP scripts):

| Setting | Default |
|---------|---------|
| `WIFI_SSID` / `WIFI_PASSWORD` | Must match `wifi.ssid` / `wifi.password` in `rpi_sound_trigger/config.yaml` (defaults: `ESP32-SHOW` / `showtime1`) |
| `MQTT_HOST` | `192.168.4.1` |
| `MQTT_TOPIC` | `displays/trigger` |
| `MQTT_QOS` | `1` (at-least-once; match Pi `mqtt.qos`) |
| `MEDIA_IDLE_PATH` | `/mjpeg/idle.mjpeg` |
| `MEDIA_ALERT_PATH` | `/mjpeg/alert.mjpeg` |

All boards subscribe to the same topic, so one mic trigger updates every display at once.

## Video conversion

Videos must be **MJPEG** (a stream of JPEG frames). Convert with ffmpeg.

MJPEG has **no inter-frame compression**, so long clips get large quickly. File size is driven by **resolution × fps × JPEG quality (`-q:v`) × duration**. Higher `-q:v` = lower quality = smaller files (typical range ~2–31).

Set `MJPEG_FRAME_RATE` in `app_config.h` to the same `fps=` value you encode with — A/V sync uses that constant.

### Long clips (working preset)

For **~8–9 minute** videos, this balance keeps A/V sync usable and MJPEG around **~100 MB** at 640×384:

```bash
# Long clip — 8 fps, low JPEG quality, compact MP3 (set MJPEG_FRAME_RATE to 8)
ffmpeg -i input.mp4 -filter_complex "[0:v]scale=640:384:force_original_aspect_ratio=decrease,fps=8[v]" -map "[v]" -q:v 30 -an output.mjpeg -map 0:a? -c:a libmp3lame -ar 16000 -ac 1 -b:a 32k -shortest output.mp3
```

Copy to the SD card as a matching pair (`idle.mjpeg` / `alert.mjpeg` in MQTT mode):

```
/mjpeg
  output.mjpeg
  output.mp3
```

The player prefers `.mp3` over `.wav` when both exist.

### Higher-quality presets (shorter clips)

Playback speed is dominated by **software JPEG decode**, which scales with pixel count. These presets target nicer picture / smoother motion; expect **much larger** files than the long-clip command above.

| Preset | ffmpeg `scale` | Encode fps | `-q:v` | Notes |
| ------ | -------------- | ---------- | ------ | ----- |
| **Long clip (recommended for 8–9 min)** | `640:384` | **8** | **30** | ~100 MB for long videos; best size/sync trade-off |
| Full quality | `800:480` | 12 | 9 | Native panel size; largest files |
| Balanced short | `640:384` | 12 | 9 | Good quality for short clips |
| Smoothest decode | `480:288` | 12 | 9 | Softest look; fastest decode on-device |

Video-only examples (no audio):

```bash
# Full quality — native panel resolution
ffmpeg -i input.mp4 -vf "scale=800:480:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output.mjpeg

# Balanced short — higher quality / smoother than the long-clip preset
ffmpeg -i input.mp4 -vf "scale=640:384:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output_640.mjpeg

# Smoothest — highest frame rate on-device, upscaled (slightly soft) on the 800x480 panel
ffmpeg -i input.mp4 -vf "scale=480:288:force_original_aspect_ratio=decrease,fps=12" -q:v 9 output_480.mjpeg
```

Same presets with synced MP3 (set `MJPEG_FRAME_RATE` to match `fps=`):

```bash
# Higher quality short clip — 12 fps + compact MP3
ffmpeg -i input.mp4 -filter_complex "[0:v]scale=640:384:force_original_aspect_ratio=decrease,fps=12[v]" -map "[v]" -q:v 9 -an output.mjpeg -map 0:a? -c:a libmp3lame -ar 16000 -ac 1 -b:a 32k -shortest output.mp3
```

Lower-resolution frames are centered on the panel (letterboxed at native size — no hardware upscaling), so they appear smaller/softer but decode much faster.

### Audio notes

The ESP32-8048S070 has an onboard **NS4168 I2S amplifier**.

**Recommended: MP3** — much smaller than WAV, low SD bandwidth (stable with video). The player decodes MP3 on the fly with [minimp3](https://github.com/lieff/minimp3).

**Optional: WAV** — 16-bit PCM mono at **16 kHz** or **8 kHz** (smaller). Large WAV files (>5 MB) stream from SD and may crash; prefer MP3 for long clips.

**Size guide** (audio only, ~4.5 min):

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

Tips:

- Keep resolution at or below **800×480**.
- For long videos, prefer **8 fps + high `-q:v` (e.g. 30)** over high-quality 12 fps encodes.
- For short clips, **12 fps + `-q:v` 9–13** looks better if SD space allows.
- Raise `-q:v` (e.g. 20–30) if files are too large or SD reads stutter.
- You can also use the [Video Conversion Studio](https://thelastoutpostworkshop.github.io/video_conversion/) web tool.

## Performance notes

The player overlaps work across both ESP32-S3 cores: a reader task (pinned to core 0) pulls complete JPEG frames off the SD card into a small ring of PSRAM buffers, while the loop core decodes and draws them. SD read time is therefore hidden behind decode time. Per-file serial output reports `wait` / `decode+draw` / `flush` milliseconds — `wait` near zero means decode is the bottleneck (expected); a large `wait` means the SD reader can't keep up (try a higher `-q:v` or lower resolution/fps).

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
| Idle clip    | `/mjpeg/idle.mjpeg` |
| Alert clip   | `/mjpeg/alert.mjpeg` |
| MQTT mode    | `true`             |
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
| MISSING idle.mjpeg / alert.mjpeg | MQTT mode — copy those two files into `/mjpeg` |


Serial output (115200 baud) logs playback stats per file, plus WiFi/MQTT connect lines when trigger mode is on.

## License

MIT — MJPEG parser adapted from the [ST7701 movie player](../ESP32-S3_3_16_ST7701_movie_player) project.