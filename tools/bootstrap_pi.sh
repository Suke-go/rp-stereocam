#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "warning: expected Raspberry Pi OS 64-bit/aarch64, got $(uname -m)" >&2
fi

if [[ "${EUID}" -eq 0 ]]; then
  echo "run this as the normal Pi user, not root" >&2
  exit 1
fi

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  ca-certificates \
  cmake \
  git \
  libcamera-dev \
  libdrm-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libjpeg-dev \
  ninja-build \
  pkg-config \
  v4l-utils \
  gstreamer1.0-tools \
  gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly

if apt-cache show rpicam-apps-lite >/dev/null 2>&1; then
  sudo apt-get install -y rpicam-apps-lite
elif apt-cache show libcamera-apps-lite >/dev/null 2>&1; then
  sudo apt-get install -y libcamera-apps-lite
else
  echo "warning: no rpicam/libcamera apps package found in apt cache" >&2
fi

boot_config="/boot/firmware/config.txt"
if [[ ! -f "${boot_config}" ]]; then
  boot_config="/boot/config.txt"
fi

if [[ -f "${boot_config}" ]]; then
  if ! grep -Eq '^[[:space:]]*camera_auto_detect=1' "${boot_config}"; then
    echo "camera_auto_detect=1" | sudo tee -a "${boot_config}" >/dev/null
  fi
  if grep -Eq '^[[:space:]]*start_x=1' "${boot_config}"; then
    echo "warning: legacy camera start_x=1 is enabled; remove it for libcamera/rpicam" >&2
  fi
else
  echo "warning: boot config not found; could not verify camera_auto_detect" >&2
fi

if systemctl list-unit-files ssh.service >/dev/null 2>&1; then
  sudo systemctl enable --now ssh
fi

echo "bootstrap complete. Reboot before camera validation if config.txt changed."
