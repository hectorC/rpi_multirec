#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

section() {
  echo
  echo "== $1 =="
}

section "Binary"
if [[ -x "${REPO_DIR}/build/rpi_multirec" ]]; then
  echo "OK: ${REPO_DIR}/build/rpi_multirec"
else
  echo "MISSING: ${REPO_DIR}/build/rpi_multirec"
fi

section "Recordings directory"
if [[ -d "/srv/rpi_multirec/recordings" ]]; then
  ls -ld /srv/rpi_multirec/recordings
else
  echo "MISSING: /srv/rpi_multirec/recordings"
fi

section "SPI / I2C devices"
ls -l /dev/spidev0.0 /dev/i2c-1 2>/dev/null || true

section "Services"
for svc in smbd avahi-daemon udisks2 rpi_multirec.service; do
  if systemctl list-unit-files | grep -q "^${svc}"; then
    systemctl status "${svc}" --no-pager || true
  else
    echo "Not installed: ${svc}"
  fi
  echo
  done

section "Automount units"
ls -l /etc/udev/rules.d/99-rpi_multirec-automount.rules /etc/systemd/system/rpi-usb-automount@.service 2>/dev/null || true

section "Hostname / network discovery"
hostnamectl --static || true
avahi-resolve-host-name "$(hostname).local" 2>/dev/null || true

section "ALSA devices"
arecord -l || true

section "Recorder device listing"
if [[ -x "${REPO_DIR}/build/rpi_multirec" ]]; then
  "${REPO_DIR}/build/rpi_multirec" --list-devices || true
fi
