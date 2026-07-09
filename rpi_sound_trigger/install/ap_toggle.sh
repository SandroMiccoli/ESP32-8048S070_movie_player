#!/usr/bin/env bash
# Quickly start or stop the ESP32 WiFi Access Point on Raspberry Pi 3.
#
# First-time setup (writes hostapd/dnsmasq config):  sudo bash install/setup_ap.sh
#
# Usage:
#   sudo bash install/ap_toggle.sh on
#   sudo bash install/ap_toggle.sh off
#   sudo bash install/ap_toggle.sh off --wifi "HOME_SSID" --password "HOME_PASS"
#   HOME_SSID=MyNet HOME_PASSWORD=secret sudo bash install/ap_toggle.sh off
#   sudo bash install/ap_toggle.sh status
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONFIG_FILE="${AP_CONFIG:-${SCRIPT_DIR}/../config.yaml}"
NM_UNMANAGED="/etc/NetworkManager/conf.d/99-unmanaged-wlan.conf"
HOSTAPD_CONF="/etc/hostapd/hostapd.conf"

yaml_get() {
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

IFACE="${AP_IFACE:-$(yaml_get "$CONFIG_FILE" wifi iface wlan0)}"
AP_IP="${AP_IP:-$(yaml_get "$CONFIG_FILE" wifi ip 192.168.4.1)}"
SSID="${AP_SSID:-$(yaml_get "$CONFIG_FILE" wifi ssid ESP32-SHOW)}"

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "Run as root: sudo $0 $*" >&2
    exit 1
  fi
}

service_active() {
  systemctl is-active --quiet "$1" 2>/dev/null
}

ap_on() {
  if [[ ! -f "$HOSTAPD_CONF" ]]; then
    echo "AP not configured yet. Run first: sudo bash ${SCRIPT_DIR}/setup_ap.sh" >&2
    exit 1
  fi

  echo "Starting Access Point on ${IFACE} (${SSID} @ ${AP_IP})..."

  if systemctl is-active --quiet NetworkManager 2>/dev/null; then
    mkdir -p /etc/NetworkManager/conf.d
    cat >"$NM_UNMANAGED" <<EOF
[keyfile]
unmanaged-devices=interface-name:${IFACE}
EOF
    systemctl restart NetworkManager
    sleep 2
  fi

  ip link set "${IFACE}" up || true
  ip addr flush dev "${IFACE}" 2>/dev/null || true
  ip addr add "${AP_IP}/24" dev "${IFACE}" 2>/dev/null || true

  systemctl enable hostapd dnsmasq
  systemctl restart dnsmasq
  systemctl restart hostapd

  echo
  echo "Access Point is ON:"
  echo "  SSID:  ${SSID}"
  echo "  IP:    ${AP_IP}  (MQTT broker)"
}

ap_off() {
  local wifi_ssid="${HOME_SSID:-}"
  local wifi_pass="${HOME_PASSWORD:-}"

  while [[ $# -gt 0 ]]; do
    case "$1" in
      --wifi)
        wifi_ssid="${2:-}"
        shift 2
        ;;
      --password)
        wifi_pass="${2:-}"
        shift 2
        ;;
      *)
        echo "Unknown option: $1" >&2
        exit 1
        ;;
    esac
  done

  echo "Stopping Access Point on ${IFACE}..."

  systemctl stop hostapd dnsmasq 2>/dev/null || true
  systemctl disable hostapd dnsmasq 2>/dev/null || true

  rm -f "$NM_UNMANAGED"

  if systemctl is-active --quiet NetworkManager 2>/dev/null; then
    systemctl restart NetworkManager
    sleep 2
  fi

  ip addr flush dev "${IFACE}" 2>/dev/null || true

  echo "Access Point is OFF."

  if [[ -n "$wifi_ssid" ]]; then
    if ! command -v nmcli >/dev/null 2>&1; then
      echo "nmcli not found — connect to home WiFi manually." >&2
      exit 0
    fi
    echo "Connecting to WiFi: ${wifi_ssid}"
    if [[ -n "$wifi_pass" ]]; then
      nmcli device wifi connect "$wifi_ssid" password "$wifi_pass" ifname "$IFACE"
    else
      nmcli device wifi connect "$wifi_ssid" ifname "$IFACE"
    fi
  else
    echo "To join home WiFi: sudo HOME_SSID=MyNet HOME_PASSWORD=secret $0 off"
    echo "  or: sudo $0 off --wifi MyNet --password secret"
  fi
}

ap_status() {
  local hostapd_state dnsmasq_state nm_state ip_line
  if service_active hostapd; then
    hostapd_state="active"
  else
    hostapd_state="inactive"
  fi
  if service_active dnsmasq; then
    dnsmasq_state="active"
  else
    dnsmasq_state="inactive"
  fi
  if [[ -f "$NM_UNMANAGED" ]]; then
    nm_state="wlan unmanaged (AP mode)"
  else
    nm_state="wlan managed by NetworkManager"
  fi

  ip_line="$(ip -4 addr show dev "${IFACE}" 2>/dev/null | awk '/inet / {print $2}' | head -n1 || true)"

  echo "Interface: ${IFACE}"
  echo "Config:    ${CONFIG_FILE}"
  echo "SSID:      ${SSID}"
  echo "AP IP:     ${AP_IP}"
  echo "IPv4:      ${ip_line:-(none)}"
  echo "hostapd:   ${hostapd_state}"
  echo "dnsmasq:   ${dnsmasq_state}"
  echo "Network:   ${nm_state}"

  if [[ "$hostapd_state" == "active" ]]; then
    echo
    echo "Mode: Access Point ON"
  else
    echo
    echo "Mode: Access Point OFF (client / home WiFi)"
  fi
}

usage() {
  cat <<EOF
Usage: sudo $0 <on|off|status> [options]

Commands:
  on, start, enable     Start the ESP32 Access Point (hostapd + dnsmasq)
  off, stop, disable    Stop the AP and return wlan to NetworkManager
  status                Show AP / interface state

Options (for off):
  --wifi SSID           Connect to home WiFi after stopping AP
  --password PASS       Home WiFi password

Environment (for off):
  HOME_SSID, HOME_PASSWORD

First-time AP setup: sudo bash ${SCRIPT_DIR}/setup_ap.sh
EOF
}

main() {
  local cmd="${1:-}"
  shift || true

  case "$cmd" in
    on|start|enable)
      require_root
      ap_on
      ;;
    off|stop|disable)
      require_root
      ap_off "$@"
      ;;
    status)
      ap_status
      ;;
    -h|--help|help|"")
      usage
      ;;
    *)
      echo "Unknown command: $cmd" >&2
      usage >&2
      exit 1
      ;;
  esac
}

main "$@"
