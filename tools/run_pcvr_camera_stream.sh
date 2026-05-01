#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
config_file="${script_dir}/metapuppet_pi.env"

if [[ -f "${config_file}" ]]; then
  # shellcheck disable=SC1090
  source "${config_file}"
fi

default_host="${METAPUPPET_PC_HOST:-}"
if [[ -z "${default_host}" ]]; then
  default_host="$(ip route show default 2>/dev/null | awk 'NR == 1 { print $3 }')"
fi
if [[ -z "${default_host}" ]]; then
  default_host="192.168.137.1"
fi

host="${1:-${default_host}}"
port="${2:-${METAPUPPET_PC_PORT:-5004}}"
eye_width="${3:-640}"
eye_height="${4:-360}"
fps="${5:-30}"
left_camera="${6:-${METAPUPPET_LEFT_CAMERA:-1}}"
right_camera="${7:-${METAPUPPET_RIGHT_CAMERA:-0}}"
max_skew_ms="${8:-8}"
codec="${9:-raw}"
bitrate_kbps="${10:-8000}"
encoder_mode="${11:-auto}"
max_pending_frames="${12:-1}"
max_encode_age_ms="${13:-100}"

sender="${repo_root}/build/pi_stream_sender/mps_libcamera_sbs_sender"
if [[ ! -x "${sender}" ]]; then
    echo "camera sender not built. Running build first." >&2
    "${script_dir}/build_pi_streaming_stack.sh"
fi

echo "Live camera libcamera Raw RGBA SBS stream"
echo "target       : ${host}:${port}"
echo "left/right   : camera ${left_camera} / camera ${right_camera}"
echo "eye format   : ${eye_width}x${eye_height}@${fps}"
echo "Unity texture: $((eye_width * 2))x${eye_height} RGBA32"
echo "sync         : libcamera single process, SensorTimestamp skew gate default 8ms"
echo "codec        : ${codec}"
if [[ "${codec}" == "h264" || "${codec}" == "H264" ]]; then
  echo "bitrate      : ${bitrate_kbps} kbps"
  echo "encoder mode : ${encoder_mode}"
  echo "latency gate : pending<=${max_pending_frames}, age<=${max_encode_age_ms}ms"
  echo "Unity texture: use native receiver H264, $((eye_width * 2))x${eye_height} BGRA32"
fi

exec "${sender}" "${host}" "${port}" "${eye_width}" "${eye_height}" "${fps}" "${left_camera}" "${right_camera}" "${max_skew_ms}" "${codec}" "${bitrate_kbps}" "${encoder_mode}" "${max_pending_frames}" "${max_encode_age_ms}"
