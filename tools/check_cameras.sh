#!/usr/bin/env bash
set -euo pipefail

camera_cmd=""
if command -v rpicam-hello >/dev/null 2>&1; then
  camera_cmd="rpicam-hello"
elif command -v libcamera-hello >/dev/null 2>&1; then
  camera_cmd="libcamera-hello"
else
  echo "rpicam-hello/libcamera-hello not found" >&2
  exit 1
fi

echo "kernel: $(uname -a)"
echo "camera command: ${camera_cmd}"
echo
"${camera_cmd}" --list-cameras

echo
echo "left camera smoke test"
"${camera_cmd}" --camera 0 --timeout 1000 --nopreview

echo
echo "right camera smoke test"
"${camera_cmd}" --camera 1 --timeout 1000 --nopreview

echo
echo "camera validation complete"
