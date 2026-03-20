#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
RUN_USER="${SUDO_USER:-${USER:-}}"
SAMBA_USER="recshare"
HOSTNAME_TARGET="rpirec"
RECORDER_ARGS="--hat-ui --mic spcmic"
WITH_SAMBA=1
WITH_AUTOMOUNT=1
ENABLE_BOOT_SERVICE=1
START_SERVICE=0
BUILD_PROJECT=1
ENABLE_SPI_I2C=1

usage() {
  cat <<'EOF'
Usage: sudo ./scripts/install_rpi.sh [options]

Options:
  --repo-dir <path>        Repository root to use for build/service paths
  --run-user <user>        Non-root user that owns the build directory
  --samba-user <user>      Samba user for the recordings share (default: recshare)
  --hostname <name>        Hostname for Avahi/Samba discovery (default: rpirec)
  --recorder-args <args>   Arguments passed to rpi_multirec.service
  --no-samba               Skip Samba + Avahi setup
  --no-automount           Skip exFAT USB automount setup
  --no-boot-service        Skip boot service installation
  --no-build               Skip cmake configure/build
  --no-spi-i2c             Skip raspi-config SPI/I2C enablement
  --start-service          Start rpi_multirec.service after installation
  -h, --help               Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-dir)
      REPO_DIR="$2"
      shift 2
      ;;
    --run-user)
      RUN_USER="$2"
      shift 2
      ;;
    --samba-user)
      SAMBA_USER="$2"
      shift 2
      ;;
    --hostname)
      HOSTNAME_TARGET="$2"
      shift 2
      ;;
    --recorder-args)
      RECORDER_ARGS="$2"
      shift 2
      ;;
    --no-samba)
      WITH_SAMBA=0
      shift
      ;;
    --no-automount)
      WITH_AUTOMOUNT=0
      shift
      ;;
    --no-boot-service)
      ENABLE_BOOT_SERVICE=0
      shift
      ;;
    --no-build)
      BUILD_PROJECT=0
      shift
      ;;
    --no-spi-i2c)
      ENABLE_SPI_I2C=0
      shift
      ;;
    --start-service)
      START_SERVICE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

REPO_DIR="$(cd "${REPO_DIR}" && pwd)"
if [[ ! -f "${REPO_DIR}/CMakeLists.txt" ]]; then
  echo "Repository root does not look correct: ${REPO_DIR}" >&2
  exit 1
fi

if [[ -z "${RUN_USER}" ]] || ! id -u "${RUN_USER}" >/dev/null 2>&1; then
  echo "Could not determine a valid non-root run user. Use --run-user <user>." >&2
  exit 1
fi

log() {
  echo "[install_rpi] $*"
}

install_packages() {
  local packages=(
    build-essential
    cmake
    pkg-config
    libasound2-dev
    libsndfile1-dev
    libgpiod-dev
    i2c-tools
  )

  if [[ ${WITH_SAMBA} -eq 1 ]]; then
    packages+=(samba avahi-daemon avahi-utils smbclient libnss-mdns)
  fi
  if [[ ${WITH_AUTOMOUNT} -eq 1 ]]; then
    packages+=(udisks2 exfatprogs)
  fi

  log "Installing packages"
  apt-get update
  apt-get install -y "${packages[@]}"
}

enable_interfaces() {
  if [[ ${ENABLE_SPI_I2C} -eq 0 ]]; then
    return
  fi
  if command -v raspi-config >/dev/null 2>&1; then
    log "Enabling SPI and I2C with raspi-config"
    raspi-config nonint do_spi 0 || true
    raspi-config nonint do_i2c 0 || true
  else
    log "raspi-config not found; skipping SPI/I2C enablement"
  fi
}

setup_recordings_dir() {
  log "Creating internal recordings directory"
  mkdir -p /srv/rpi_multirec/recordings
  chown root:root /srv/rpi_multirec/recordings
  chmod 755 /srv/rpi_multirec/recordings
}

install_samba_block() {
  local smb_conf="/etc/samba/smb.conf"
  local begin="# BEGIN rpi_multirec"
  local end="# END rpi_multirec"
  local block_file
  block_file="$(mktemp)"
  cp "${REPO_DIR}/deploy/samba/recordings-share.conf" "${block_file}"
  sed -i "s/@SAMBA_USER@/${SAMBA_USER}/g" "${block_file}"

  if [[ -f "${smb_conf}" && ! -f "${smb_conf}.rpi_multirec.bak" ]]; then
    cp "${smb_conf}" "${smb_conf}.rpi_multirec.bak"
  fi

  python3 - "$smb_conf" "$block_file" "$begin" "$end" <<'PY'
from pathlib import Path
import sys

smb_conf = Path(sys.argv[1])
block_file = Path(sys.argv[2])
begin = sys.argv[3]
end = sys.argv[4]
block = block_file.read_text(encoding='utf-8').strip() + "\n"
text = smb_conf.read_text(encoding='utf-8') if smb_conf.exists() else "[global]\n"
managed = begin + "\n" + block + end + "\n"
if begin in text and end in text:
    start = text.index(begin)
    finish = text.index(end, start) + len(end)
    if finish < len(text) and text[finish:finish + 1] == "\n":
        finish += 1
    text = text[:start] + managed + text[finish:]
else:
    if not text.endswith("\n"):
        text += "\n"
    text += "\n" + managed
smb_conf.write_text(text, encoding='utf-8')
PY

  rm -f "${block_file}"
}

setup_samba() {
  if [[ ${WITH_SAMBA} -eq 0 ]]; then
    return
  fi

  log "Configuring Samba and Avahi"
  hostnamectl set-hostname "${HOSTNAME_TARGET}"

  if ! id -u "${SAMBA_USER}" >/dev/null 2>&1; then
    adduser --disabled-password --gecos "" "${SAMBA_USER}"
  fi

  chgrp "${SAMBA_USER}" /srv/rpi_multirec/recordings
  chmod 2775 /srv/rpi_multirec/recordings

  install_samba_block

  if ! pdbedit -L 2>/dev/null | cut -d: -f1 | grep -qx "${SAMBA_USER}"; then
    log "Create a Samba password for ${SAMBA_USER}"
    smbpasswd -a "${SAMBA_USER}"
  fi

  testparm -s >/dev/null
  systemctl enable --now smbd avahi-daemon
  systemctl restart smbd avahi-daemon
}

setup_automount() {
  if [[ ${WITH_AUTOMOUNT} -eq 0 ]]; then
    return
  fi

  log "Installing USB exFAT automount support"
  install -D -m 0644 \
    "${REPO_DIR}/deploy/udev/99-rpi_multirec-automount.rules" \
    "/etc/udev/rules.d/99-rpi_multirec-automount.rules"
  install -D -m 0644 \
    "${REPO_DIR}/deploy/systemd/rpi-usb-automount@.service" \
    "/etc/systemd/system/rpi-usb-automount@.service"

  systemctl enable --now udisks2
  udevadm control --reload
  systemctl daemon-reload
}

build_project() {
  if [[ ${BUILD_PROJECT} -eq 0 ]]; then
    return
  fi

  log "Configuring and building as ${RUN_USER}"
  sudo -u "${RUN_USER}" cmake -S "${REPO_DIR}" -B "${REPO_DIR}/build"
  sudo -u "${RUN_USER}" cmake --build "${REPO_DIR}/build" -j
}

install_boot_service() {
  if [[ ${ENABLE_BOOT_SERVICE} -eq 0 ]]; then
    return
  fi

  log "Installing systemd boot service"
  local tmp
  tmp="$(mktemp)"
  sed \
    -e "s|@REPO_DIR@|${REPO_DIR}|g" \
    -e "s|@RECORDER_ARGS@|${RECORDER_ARGS}|g" \
    "${REPO_DIR}/deploy/systemd/rpi_multirec.service.in" > "${tmp}"
  install -D -m 0644 "${tmp}" "/etc/systemd/system/rpi_multirec.service"
  rm -f "${tmp}"

  systemctl daemon-reload
  systemctl enable rpi_multirec.service
  if [[ ${START_SERVICE} -eq 1 ]]; then
    systemctl restart rpi_multirec.service
  fi
}

install_packages
enable_interfaces
setup_recordings_dir
setup_samba
setup_automount
build_project
install_boot_service

log "Done"
log "Run ./scripts/postinstall_check.sh for a quick verification pass."
if [[ ${ENABLE_BOOT_SERVICE} -eq 1 && ${START_SERVICE} -eq 0 ]]; then
  log "Boot service installed but not started. Start it with: sudo systemctl start rpi_multirec.service"
fi
