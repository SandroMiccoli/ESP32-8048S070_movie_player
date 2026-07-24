# RPi Sound Trigger

Mic (webcam or USB) → volume threshold → MQTT `alert` for every ESP32 display on the Pi access point.

Designed for **Raspberry Pi 3** on **Raspbian / Debian 13 (trixie)** with Python 3.

## Existing install — what to change

Pull/sync these updated files onto the Pi, then:

1. **`config.yaml`** — use `display.mode: auto` (already the default in the new config). Remove old `display.enabled` if you still have it.
2. **Reinstall the systemd unit** (the old one forced `--no-display`):

```bash
cd ~/ESP32-8048S070_movie_player/rpi_sound_trigger
# Edit User/paths/XDG_RUNTIME_DIR if your username is not pi (uid 1000)
sudo cp install/sound-trigger.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl restart sound-trigger.service
```

3. **Screen + desktop** — plug in HDMI. On Pi OS with desktop, keep a logged-in session on `:0` (enable auto-login). On **Lite** (no desktop), set in `config.yaml`:

```yaml
display:
  mode: auto
  sdl_driver: kmsdrm
```

4. Confirm:

```bash
journalctl -u sound-trigger -f
# Expect: Display: connected: HDMI-A-1   and   UI mode: on-screen VU
```

Unplug the screen (or boot without HDMI) and it should log `UI mode: headless`.

---

## Architecture

1. Pi creates a WiFi **Access Point** (`hostapd` + `dnsmasq`).
2. **Mosquitto** listens on `192.168.4.1:1883`.
3. This app monitors the mic and publishes `{"state":"alert",...}` to `displays/trigger`.
4. All ESP32 boards join the AP, subscribe to the topic, and switch `idle.mjpeg` → `alert.mjpeg`.

---

## Quick install (on the Pi)

Do the Python/`apt` steps while the Pi has **internet** (home WiFi). The AP is isolated and blocks package downloads — set it up **last** (or stop it temporarily; see [Toggle AP ↔ home WiFi](#toggle-ap--home-wifi)).

```bash
# Go to the project folder (adjust path if you cloned elsewhere)
cd ~/ESP32-8048S070_movie_player/rpi_sound_trigger
# or: cd ~/rpi_sound_trigger
```

### 0) Edit WiFi credentials (before AP setup)

```bash
nano config.yaml
```

Set `wifi.ssid` and `wifi.password` — they must match `WIFI_SSID` / `WIFI_PASSWORD` in the ESP32 `app_config.h`.

### 1) System packages (run once, needs internet)

```bash
sudo bash install/setup_python_deps.sh
```

Installs build headers, PortAudio, SDL, and `libopenblas0-pthread` (required by pip’s NumPy wheel).

### 2) MQTT broker

```bash
sudo bash install/setup_mqtt.sh
```

### 3) Python virtualenv

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install -r requirements.txt
```

### 4) Pick the mic / webcam

```bash
source .venv/bin/activate
python src/main.py --list-devices
```

Edit `config.yaml` → `audio.device`:

- `null` — default input
- `"USB"` — name substring (preferred; stable across reboots)
- `2` — numeric index (can change after reboot)

### 5) WiFi AP for ESP32 clients (do this **after** pip works)

```bash
sudo bash install/setup_ap.sh
```

Reads `wifi.*` from `config.yaml`. Env overrides still work:

```bash
sudo AP_SSID=MinhaRede AP_PASSWORD=minhasenha bash install/setup_ap.sh
```

### 6) Make the AP start reliably on every boot (recommended)

`setup_ap.sh` enables `hostapd` + `dnsmasq`, but on modern Pi OS they often lose a boot race with NetworkManager / `wlan0` and stay down. Harden `hostapd` so it waits, assigns the AP IP, and retries:

```bash
# Leave AP mode as the boot default
sudo bash install/ap_toggle.sh on

sudo mkdir -p /etc/systemd/system/hostapd.service.d
sudo tee /etc/systemd/system/hostapd.service.d/override.conf >/dev/null <<'EOF'
[Unit]
After=network.target sys-subsystem-net-devices-wlan0.device
Wants=sys-subsystem-net-devices-wlan0.device

[Service]
Restart=on-failure
RestartSec=3
ExecStartPre=/bin/sleep 2
ExecStartPre=/sbin/ip link set wlan0 up
ExecStartPre=-/sbin/ip addr flush dev wlan0
ExecStartPre=/sbin/ip addr add 192.168.4.1/24 dev wlan0
EOF

sudo systemctl daemon-reload
sudo systemctl enable hostapd dnsmasq
sudo systemctl restart hostapd dnsmasq
```

If your AP IP in `config.yaml` is not `192.168.4.1`, change that address in the override to match `wifi.ip`.

Reboot once and confirm:

```bash
systemctl status hostapd --no-pager
sudo bash install/ap_toggle.sh status
```

**Note:** `ap_toggle.sh off` disables `hostapd`/`dnsmasq` for home WiFi. After using home WiFi, run `sudo bash install/ap_toggle.sh on` again before the next show reboot.

### 7) Auto-start the sound trigger on boot

Edit paths/user in `install/sound-trigger.service` if your clone or username differ, then:

```bash
sudo cp install/sound-trigger.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sound-trigger.service
```

---

## How to run

Always activate the venv first:

```bash
cd ~/ESP32-8048S070_movie_player/rpi_sound_trigger   # or your clone path
source .venv/bin/activate
```

### Normal (auto UI / headless)

By default the app **detects a connected screen** (HDMI/DSI via DRM):

- **Screen connected** → fullscreen VU meter with live mic amplitude
- **No screen** → headless terminal readout

```bash
python src/main.py
```

Yell into the mic and watch the bar cross the yellow threshold line. Tune `audio.threshold_dbfs` in `config.yaml` to sit just below your peak.

### Force modes

```bash
python src/main.py --display      # always open the VU UI
python src/main.py --no-display   # always headless
```

### Custom config path

```bash
python src/main.py -c /path/to/config.yaml
```

### Systemd (production)

```bash
sudo systemctl start sound-trigger
sudo systemctl status sound-trigger
journalctl -u sound-trigger -f
```

The service starts **without** `--no-display`, so HDMI shows the VU automatically. If your username is not `pi` or the project path differs, edit `install/sound-trigger.service` before copying it.

---

## Simple tests

Run these after install, before relying on the full ESP32 setup.

### 1) List input devices

```bash
source .venv/bin/activate
python src/main.py --list-devices
```

Confirm your webcam/USB mic appears. Set `audio.device` in `config.yaml` if the default is wrong.

### 2) MQTT broker smoke test

Terminal A — subscribe:

```bash
mosquitto_sub -h 127.0.0.1 -t 'displays/trigger' -v
```

Terminal B — publish a fake alert:

```bash
mosquitto_pub -h 127.0.0.1 -t 'displays/trigger' -q 1 -m '{"state":"alert","ts":0}'
```

Terminal A should print the message. If this fails, fix Mosquitto before testing the mic app.

### 3) Mic + MQTT (headless)

```bash
source .venv/bin/activate
python src/main.py --no-display
```

Clap or yell past the threshold. You should see `[trigger] level=… dBFS publish=ok` in the terminal.

### 4) Multi-board (with ESP32s)

1. Flash all ESP32s with the same WiFi/MQTT settings and SD layout (`/mjpeg/idle.mjpeg`, `/mjpeg/alert.mjpeg`).
2. Power them so they join the Pi AP (Serial should log WiFi + MQTT connected).
3. Run this app; raise volume past threshold (or use the `mosquitto_pub` command above).
4. Every display should switch to alert simultaneously, then return to idle when the clip ends.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `Python.h: No such file` or `Package libffi was not found` during `pip install` | Skipped step 1 — run `sudo bash install/setup_python_deps.sh` and retry `pip install -r requirements.txt` |
| `libopenblas.so.0: cannot open shared object file` when running `main.py` | `sudo apt-get install -y libopenblas0-pthread` (or re-run `setup_python_deps.sh`, which includes it) |
| `libopenblas0-pthread` not found | Try `sudo apt-get install -y libopenblas0` |
| `pip install` fails while on the Pi AP only | Stop AP and reconnect to home WiFi: `sudo bash install/ap_toggle.sh off --wifi "HOME_SSID" --password "PASS"` |
| Wrong mic / no audio | Run `--list-devices`, set `audio.device` to a name substring (e.g. `"USB"`) |
| ESP32s don’t connect | Confirm `config.yaml` `wifi.ssid`/`password` match `app_config.h`; re-run `setup_ap.sh` after changes |
| AP does not come up after reboot (`hostapd` failed/inactive) | Follow [step 6](#6-make-the-ap-start-reliably-on-every-boot-recommended); check `journalctl -u hostapd -b`. If you used `ap_toggle.sh off`, run `ap_toggle.sh on` again |
| No VU on HDMI | Confirm cable/hotplug (`cat /sys/class/drm/*/status`); ensure desktop is logged in or set `display.sdl_driver: kmsdrm`; reinstall service without `--no-display` |
| VU fails under systemd | Check `DISPLAY=:0` / `XAUTHORITY` / uid in `XDG_RUNTIME_DIR` match your user; user must be in `video`/`render` groups |

---

## MQTT contract

| Field | Value |
|-------|--------|
| Topic | `displays/trigger` |
| QoS | 1 (at-least-once; Pi publish + ESP32 subscribe) |
| Payload | `{"state":"alert","ts":<unix>,"level_dbfs":-12.3}` |

ESP32s ignore unknown fields; only `state` is required. After playing `alert.mjpeg` once, each board returns to looping `idle.mjpeg`.

---

## Config (`config.yaml`)

| Key | Meaning |
|-----|---------|
| `wifi.ssid` | Access Point name (must match ESP32 `WIFI_SSID`) |
| `wifi.password` | WPA2 passphrase, ≥ 8 chars (must match ESP32 `WIFI_PASSWORD`) |
| `wifi.ip` | AP / MQTT host IP (default `192.168.4.1`) |
| `audio.threshold_dbfs` | Trigger when level ≥ this (e.g. `-25`; louder ≈ closer to `0`) |
| `audio.cooldown_s` | Ignore further triggers after a publish (default `2.5`) |
| `audio.device` | `null`, device index, or name substring |
| `mqtt.host` | Broker IP — usually the same as `wifi.ip` |
| `mqtt.qos` | Publish QoS (default `1`; must match ESP32 `MQTT_QOS`) |
| `display.mode` | `auto` (UI if screen connected), `always`, or `never` |
| `display.fullscreen` | `null` = auto fullscreen when a screen is detected; `true`/`false` to force |
| `display.sdl_driver` | Optional SDL override (`x11`, `wayland`, `kmsdrm`) |

Tune threshold with the VU bar: yell until the bar crosses the yellow line, then set `threshold_dbfs` a little below that peak.

---

## Network notes (Pi 3)

- The Pi 3 has **one WiFi radio**: AP **or** client, not both.
- Change the network name in **`config.yaml`** (`wifi.ssid` / `wifi.password`), re-run `sudo bash install/setup_ap.sh`, and update ESP32 `app_config.h` to the same values.
- Env vars `AP_SSID` / `AP_PASSWORD` still override the YAML when running the setup script.
- AP IP from `wifi.ip` (default `192.168.4.1`) is the MQTT host used by the firmware.
- MQTT anonymous access is intentional for an isolated local AP. Do not bridge this AP to the internet without adding authentication.
- Webcam ALSA indexes can change after reboot — prefer a **name substring** in `config.yaml`.

### Toggle AP ↔ home WiFi

The Pi 3 has one WiFi radio — use `ap_toggle.sh` to switch between AP mode and home WiFi.

**Check current mode:**

```bash
sudo bash install/ap_toggle.sh status
```

**Stop AP and reconnect to home WiFi** (for `apt` / `pip`):

```bash
sudo bash install/ap_toggle.sh off --wifi "HOME_SSID" --password "HOME_PASSWORD"
# or:
HOME_SSID=MyNet HOME_PASSWORD=secret sudo bash install/ap_toggle.sh off
```

**Start AP again** (after `setup_ap.sh` has been run once):

```bash
sudo bash install/ap_toggle.sh on
```

Re-run `sudo bash install/setup_ap.sh` only if you changed `wifi.ssid` / `wifi.password` in `config.yaml`.
