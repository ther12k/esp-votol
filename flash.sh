#!/usr/bin/env bash
# Flash the VOTOL WiFi bridge firmware to a connected ESP32.
# Usage: ./flash.sh [device]     (device defaults to first ttyUSB/ttyACM found)
set -euo pipefail
cd "$(dirname "$0")"

DEVICE="${1:-}"
if [[ -z "$DEVICE" ]]; then
  DEVICE=$(ls /dev/ttyUSB0 /dev/ttyACM0 2>/dev/null | head -1 || true)
fi
if [[ -z "$DEVICE" ]]; then
  echo "No ESP32 found on /dev/ttyUSB* or /dev/ttyACM* — plug it in via USB and retry."
  exit 1
fi

# If usermod added us to dialout but this shell hasn't re-logged-in, re-exec
# through sg so the new group applies immediately (no reboot/logout needed).
if [[ ! -r "$DEVICE" || ! -w "$DEVICE" ]]; then
  if grep dialout /etc/group | grep -qw "$USER"; then
    exec sg dialout -c "cd $(printf %q "$PWD") && $(printf %q "$0") $(printf %q "$DEVICE")"
  fi
  echo "ERROR: no permission for $DEVICE (user '$USER' is not in the 'dialout' group)."
  echo "Fix once with:  sudo usermod -aG dialout \$USER   then re-run this script."
  exit 1
fi

echo "Flashing to $DEVICE ..."
exec .venv/bin/esphome run votol-wifi-bridge.yaml --device "$DEVICE"
