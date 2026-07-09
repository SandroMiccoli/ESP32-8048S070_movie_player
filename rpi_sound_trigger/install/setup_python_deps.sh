#!/usr/bin/env bash
# Install OS packages needed to pip-install rpi_sound_trigger on Raspberry Pi.
# Run this while connected to normal (internet) WiFi — before or after the AP
# is configured, but NOT while you are offline on the isolated AP only.
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y \
  build-essential \
  pkg-config \
  python3-dev \
  python3-venv \
  python3-pip \
  libffi-dev \
  libssl-dev \
  portaudio19-dev \
  libportaudio2 \
  libasound2-dev \
  libopenblas0-pthread \
  libsdl2-dev \
  libsdl2-image-dev \
  libsdl2-mixer-dev \
  libsdl2-ttf-dev

echo
echo "System packages installed."
echo "Next (as normal user, in rpi_sound_trigger/):"
echo "  python3 -m venv .venv"
echo "  source .venv/bin/activate"
echo "  pip install --upgrade pip"
echo "  pip install -r requirements.txt"
