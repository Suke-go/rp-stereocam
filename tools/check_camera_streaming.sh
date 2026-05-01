#!/usr/bin/env bash
set -euo pipefail

width="${1:-640}"
height="${2:-360}"
fps="${3:-30}"
timeout_ms="${4:-2000}"

camera_cmd=""
if command -v rpicam-vid >/dev/null 2>&1; then
  camera_cmd="rpicam-vid"
elif command -v libcamera-vid >/dev/null 2>&1; then
  camera_cmd="libcamera-vid"
else
  echo "rpicam-vid/libcamera-vid not found" >&2
  exit 1
fi

echo "streaming smoke test: ${width}x${height}@${fps}, timeout=${timeout_ms}ms"

for camera in 1 0; do
  out="/tmp/metapuppet_cam${camera}_${width}x${height}.yuv"
  log="/tmp/metapuppet_cam${camera}_${width}x${height}.log"
  rm -f "${out}" "${log}"
  echo
  echo "camera ${camera}"
  if timeout --kill-after=2s "$((timeout_ms / 1000 + 6))s" "${camera_cmd}" \
      --camera "${camera}" \
      --width "${width}" \
      --height "${height}" \
      --framerate "${fps}" \
      --codec yuv420 \
      --timeout "${timeout_ms}" \
      --nopreview \
      --output "${out}" \
      2>"${log}"; then
    ls -lh "${out}"
  else
    rc=$?
    echo "camera ${camera} failed: rc=${rc}" >&2
    cat "${log}" >&2
    exit "${rc}"
  fi
done

echo
echo "single-camera streaming smoke test complete"
