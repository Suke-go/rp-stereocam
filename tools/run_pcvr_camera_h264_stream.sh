#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

host="${1:-192.168.137.1}"
port="${2:-5004}"
eye_width="${3:-640}"
eye_height="${4:-360}"
fps="${5:-30}"
left_camera="${6:-0}"
right_camera="${7:-0}"
bitrate_kbps="${8:-8000}"
max_skew_ms="${9:-8}"
encoder_mode="${10:-auto}"
max_pending_frames="${11:-1}"
max_encode_age_ms="${12:-100}"

exec "${script_dir}/run_pcvr_camera_stream.sh" \
  "${host}" \
  "${port}" \
  "${eye_width}" \
  "${eye_height}" \
  "${fps}" \
  "${left_camera}" \
  "${right_camera}" \
  "${max_skew_ms}" \
  h264 \
  "${bitrate_kbps}" \
  "${encoder_mode}" \
  "${max_pending_frames}" \
  "${max_encode_age_ms}"
