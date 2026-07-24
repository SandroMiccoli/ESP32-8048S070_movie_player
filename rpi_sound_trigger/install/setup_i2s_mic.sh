#!/usr/bin/env bash
# Enable Raspberry Pi I2S capture for an INMP441 MEMS microphone.
# Target: Raspberry Pi 3 / Pi OS Debian 13 (trixie), headless Lite OK.
#
# Wiring (INMP441 → Pi):
#   VDD → 3V3 (pin 1 or 17)     GND → GND
#   SCK → GPIO18 (pin 12)       WS  → GPIO19 (pin 35)
#   SD  → GPIO20 (pin 38)       L/R → GND  (Left / mono)
#
# After reboot, list devices:
#   arecord -l
#   python src/main.py --list-devices
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

# Bookworm/Trixie use /boot/firmware/config.txt; older images use /boot/config.txt
CONFIG=""
for candidate in /boot/firmware/config.txt /boot/config.txt; do
  if [[ -f "${candidate}" ]]; then
    CONFIG="${candidate}"
    break
  fi
done

if [[ -z "${CONFIG}" ]]; then
  echo "Could not find boot config.txt under /boot/firmware or /boot" >&2
  exit 1
fi

echo "Using boot config: ${CONFIG}"
cp -a "${CONFIG}" "${CONFIG}.bak.$(date +%Y%m%d%H%M%S)"

# Remove prior managed lines so re-runs stay idempotent
tmp="$(mktemp)"
grep -Ev \
  '^[[:space:]]*(#[[:space:]]*)?(dtparam=i2s=|dtoverlay=googlevoicehat-soundcard)|# --- rpi_sound_trigger INMP441' \
  "${CONFIG}" >"${tmp}" || true
mv "${tmp}" "${CONFIG}"

{
  echo
  echo "# --- rpi_sound_trigger INMP441 I2S mic (setup_i2s_mic.sh) ---"
  echo "dtparam=i2s=on"
  echo "dtoverlay=googlevoicehat-soundcard"
} >>"${CONFIG}"

echo
echo "I2S mic enabled in ${CONFIG}:"
echo "  dtparam=i2s=on"
echo "  dtoverlay=googlevoicehat-soundcard"
echo
echo "Wire the INMP441 (3.3 V only):"
echo "  VDD→3V3  GND→GND  SCK→GPIO18  WS→GPIO19  SD→GPIO20  L/R→GND"
echo
echo "Reboot required:"
echo "  sudo reboot"
echo
echo "After reboot, confirm capture:"
echo "  arecord -l"
echo "  cd ~/ESP32-8048S070_movie_player/rpi_sound_trigger && source .venv/bin/activate"
echo "  python src/main.py --list-devices"
echo
echo "In config.yaml set something like:"
echo "  audio:"
echo "    device: \"voice\"    # substring of the Google voiceHAT / I2S card name"
echo "    sample_rate: 48000"
echo "    channels: 2          # overlay often exposes stereo; L channel used"
echo "    threshold_dbfs: -30.0 # retune with VU / headless level"
