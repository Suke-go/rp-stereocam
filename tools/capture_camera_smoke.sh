#!/usr/bin/env bash
set -euo pipefail

camera_cmd=""
if command -v rpicam-jpeg >/dev/null 2>&1; then
  camera_cmd="rpicam-jpeg"
elif command -v libcamera-jpeg >/dev/null 2>&1; then
  camera_cmd="libcamera-jpeg"
else
  echo "rpicam-jpeg/libcamera-jpeg not found" >&2
  exit 1
fi

out_dir="${1:-$HOME/metapuppet_camera_smoke}"
mkdir -p "${out_dir}"

echo "capturing camera 0 -> ${out_dir}/camera0.jpg"
"${camera_cmd}" --camera 0 -o "${out_dir}/camera0.jpg" --timeout 1000 --nopreview

echo "capturing camera 1 -> ${out_dir}/camera1.jpg"
"${camera_cmd}" --camera 1 -o "${out_dir}/camera1.jpg" --timeout 1000 --nopreview

ls -lh "${out_dir}/camera0.jpg" "${out_dir}/camera1.jpg"
echo "smoke capture complete"
