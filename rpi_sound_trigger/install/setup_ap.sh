#!/usr/bin/env bash
# Create a WiFi Access Point on Raspberry Pi 3 (Raspbian Trixie) for ESP32 clients.
# SSID/password come from ../config.yaml (wifi section), overridable via env vars.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG_FILE="${AP_CONFIG:-${SCRIPT_DIR}/../config.yaml}"

yaml_get() {
  # Read a top-level section.key from a simple YAML file (no nested lists).
  # Usage: yaml_get <file> <section> <key> <default>
  local file="$1" section="$2" key="$3" default="${4:-}"
  if [[ ! -f "$file" ]]; then
    printf '%s' "$default"
    return
  fi
  python3 - "$file" "$section" "$key" "$default" <<'PY'
import re, sys
path, section, key, default = sys.argv[1:5]
text = open(path, encoding="utf-8").read().splitlines()
in_section = False
pat_section = re.compile(rf"^{re.escape(section)}\s*:")
pat_key = re.compile(rf"^  {re.escape(key)}\s*:\s*(.*?)\s*(?:#.*)?$")
for line in text:
    if pat_section.match(line):
        in_section = True
        continue
    if in_section:
        if line and not line.startswith(" ") and not line.startswith("\t") and not line.strip().startswith("#"):
            break
        m = pat_key.match(line)
        if m:
            val = m.group(1).strip()
            if (val.startswith('"') and val.endswith('"')) or (val.startswith("'") and val.endswith("'")):
                val = val[1:-1]
            print(val)
            raise SystemExit(0)
print(default)
PY
}

SSID="${AP_SSID:-$(yaml_get "$CONFIG_FILE" wifi ssid ESP32-SHOW)}"
PASSPHRASE="${AP_PASSWORD:-$(yaml_get "$CONFIG_FILE" wifi password showtime1)}"
IFACE="${AP_IFACE:-$(yaml_get "$CONFIG_FILE" wifi iface wlan0)}"
AP_IP="${AP_IP:-$(yaml_get "$CONFIG_FILE" wifi ip 192.168.4.1)}"
DHCP_START="${AP_DHCP_START:-$(yaml_get "$CONFIG_FILE" wifi dhcp_start 192.168.4.50)}"
DHCP_END="${AP_DHCP_END:-$(yaml_get "$CONFIG_FILE" wifi dhcp_end 192.168.4.200)}"

if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run as root: sudo $0" >&2
  exit 1
fi

if [[ ${#PASSPHRASE} -lt 8 ]]; then
  echo "wifi.password / AP_PASSWORD must be at least 8 characters" >&2
  exit 1
fi

if [[ -z "$SSID" ]]; then
  echo "wifi.ssid is empty — set it in ${CONFIG_FILE}" >&2
  exit 1
fi

echo "Using config: ${CONFIG_FILE}"
echo "  SSID=${SSID}  iface=${IFACE}  ip=${AP_IP}"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y hostapd dnsmasq iptables

systemctl stop hostapd 2>/dev/null || true
systemctl stop dnsmasq 2>/dev/null || true
systemctl unmask hostapd 2>/dev/null || true

# Prefer classic hostapd+dnsmasq on Pi 3 — more predictable than NM hotspot.
# Stop NetworkManager from managing wlan0 if present.
if systemctl is-active --quiet NetworkManager 2>/dev/null; then
  echo "Configuring NetworkManager to ignore ${IFACE}..."
  mkdir -p /etc/NetworkManager/conf.d
  cat >/etc/NetworkManager/conf.d/99-unmanaged-wlan.conf <<EOF
[keyfile]
unmanaged-devices=interface-name:${IFACE}
EOF
  systemctl restart NetworkManager
  sleep 2
fi

# Static IP for the AP interface
mkdir -p /etc/network/interfaces.d
cat >/etc/network/interfaces.d/${IFACE}-ap <<EOF
allow-hotplug ${IFACE}
iface ${IFACE} inet static
  address ${AP_IP}
  netmask 255.255.255.0
EOF

# Bring interface up with static IP (dhcpcd or ifup)
if command -v dhcpcd >/dev/null 2>&1; then
  if ! grep -q "interface ${IFACE}" /etc/dhcpcd.conf 2>/dev/null; then
    cat >>/etc/dhcpcd.conf <<EOF

# RPi sound-trigger AP
interface ${IFACE}
static ip_address=${AP_IP}/24
nohook wpa_supplicant
EOF
  fi
  systemctl restart dhcpcd 2>/dev/null || true
fi

ip link set "${IFACE}" up || true
ip addr flush dev "${IFACE}" 2>/dev/null || true
ip addr add "${AP_IP}/24" dev "${IFACE}" 2>/dev/null || true

cat >/etc/hostapd/hostapd.conf <<EOF
interface=${IFACE}
driver=nl80211
ssid=${SSID}
hw_mode=g
channel=6
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=${PASSPHRASE}
wpa_key_mgmt=WPA-PSK
wpa_pairwise=CCMP
rsn_pairwise=CCMP
ieee80211w=0
country_code=BR
ieee80211n=0
EOF

if [[ -f /etc/default/hostapd ]]; then
  sed -i 's|^#\?DAEMON_CONF=.*|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd
fi

# Backup stock dnsmasq once
if [[ -f /etc/dnsmasq.conf && ! -f /etc/dnsmasq.conf.bak.sound-trigger ]]; then
  cp /etc/dnsmasq.conf /etc/dnsmasq.conf.bak.sound-trigger
fi

cat >/etc/dnsmasq.d/sound-trigger-ap.conf <<EOF
interface=${IFACE}
bind-interfaces
dhcp-range=${DHCP_START},${DHCP_END},255.255.255.0,24h
dhcp-option=3,${AP_IP}
dhcp-option=6,${AP_IP}
EOF

# Disable system resolver conflict if present
systemctl disable --now systemd-resolved 2>/dev/null || true

systemctl enable hostapd
systemctl enable dnsmasq
systemctl restart dnsmasq
systemctl restart hostapd

echo
echo "Access Point ready:"
echo "  SSID:       ${SSID}"
echo "  Password:   ${PASSPHRASE}"
echo "  AP IP:      ${AP_IP}  (use as MQTT_HOST on ESP32)"
echo "  DHCP:       ${DHCP_START} – ${DHCP_END}"
echo
echo "Remember: set the same values in ESP32 app_config.h:"
echo "  #define WIFI_SSID \"${SSID}\""
echo "  #define WIFI_PASSWORD \"${PASSPHRASE}\""
echo "  #define MQTT_HOST \"${AP_IP}\""
echo
echo "Next: sudo $(dirname "$0")/setup_mqtt.sh"
