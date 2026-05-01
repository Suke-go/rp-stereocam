#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

host="${1:-192.168.137.1}"
port="${2:-5004}"

exec "${script_dir}/run_pcvr_camera_h264_stream.sh" \
  "${host}" \
  "${port}" \
  320 \
  180 \
  30 \
  0 \
  0 \
  2500 \
  8 \
  software \
  1 \
  90
