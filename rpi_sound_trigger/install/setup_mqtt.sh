#!/usr/bin/env bash
# Install and configure Mosquitto MQTT broker for local AP clients.
set -euo pipefail

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y mosquitto mosquitto-clients

CONF_DIR=/etc/mosquitto/conf.d
mkdir -p "${CONF_DIR}"

# MVP: anonymous MQTT on the isolated AP LAN only.
# Tighten later (password file + allowlist) if the AP bridges to the internet.
cat >"${CONF_DIR}/sound-trigger.conf" <<'EOF'
listener 1883 0.0.0.0
allow_anonymous true
persistence false
EOF

# Drop default bind that may conflict on some images
if [[ -f /etc/mosquitto/mosquitto.conf ]]; then
  # Ensure includes are active (Debian packages usually include conf.d)
  if ! grep -q 'include_dir /etc/mosquitto/conf.d' /etc/mosquitto/mosquitto.conf; then
    echo 'include_dir /etc/mosquitto/conf.d' >>/etc/mosquitto/mosquitto.conf
  fi
fi

systemctl enable mosquitto
systemctl restart mosquitto

sleep 1
if ss -ltn | grep -q ':1883'; then
  echo "Mosquitto listening on 0.0.0.0:1883"
else
  echo "Warning: port 1883 not observed — check: journalctl -u mosquitto -e" >&2
fi

echo
echo "Smoke test (on this Pi):"
echo "  mosquitto_sub -h 127.0.0.1 -t 'displays/trigger' -v"
echo "  mosquitto_pub -h 127.0.0.1 -t 'displays/trigger' -q 1 -m '{\"state\":\"alert\",\"ts\":0}'"
