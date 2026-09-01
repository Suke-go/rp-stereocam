#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sender="${repo_root}/build/pi_stream_sender/mps_stereo_x264_sender"
config_file="${MPS_CONFIG_FILE:-${repo_root}/tools/metapuppet_pi.env}"

if [[ -f "${config_file}" ]]; then
    set -a
    # shellcheck disable=SC1090
    source "${config_file}"
    set +a
fi

pc_host="${1:-${METAPUPPET_PC_HOST:-192.168.50.1}}"
port="${2:-${METAPUPPET_PC_PORT:-5004}}"
eye_width="${3:-${METAPUPPET_EYE_WIDTH:-1280}}"
eye_height="${4:-${METAPUPPET_EYE_HEIGHT:-720}}"
fps="${5:-${METAPUPPET_FPS:-60}}"
left_camera="${6:-${METAPUPPET_LEFT_CAMERA:-0}}"
right_camera="${7:-${METAPUPPET_RIGHT_CAMERA:-1}}"

if [[ ! -x "$sender" ]]; then
    echo "Dual sender is not built: $sender" >&2
    echo "Run tools/deploy_stereo_dual.ps1 from Windows or build mps_stereo_x264_sender on this Pi." >&2
    exit 1
fi

# Camera colour/exposure is automatic unless the deployment config explicitly
# exports the corresponding MPS_CAMERA_* values. Keep recorded calibration
# values in the device-local env file, not as launcher defaults.
export MPS_CAPTURE_BACKEND="${MPS_CAPTURE_BACKEND:-libcamera}"
if { [[ -n "${MPS_CAMERA_SHUTTER_US:-}" ]] &&
     [[ -z "${MPS_CAMERA_ANALOG_GAIN:-}" ]]; } ||
   { [[ -z "${MPS_CAMERA_SHUTTER_US:-}" ]] &&
     [[ -n "${MPS_CAMERA_ANALOG_GAIN:-}" ]]; }; then
    echo "warning: fixed exposure needs both MPS_CAMERA_SHUTTER_US and MPS_CAMERA_ANALOG_GAIN; sender will keep AE enabled" >&2
fi

export MPS_X264_CRF="${MPS_X264_CRF:-20}"
export MPS_X264_KEYINT="${MPS_X264_KEYINT:-30}"
export MPS_ADAPTIVE="${MPS_ADAPTIVE:-1}"
export MPS_ADAPTIVE_CRF_MAX="${MPS_ADAPTIVE_CRF_MAX:-30}"
export MPS_CAMERA_BUFFERS="${MPS_CAMERA_BUFFERS:-6}"
export MPS_KEYFRAME_NACK="${MPS_KEYFRAME_NACK:-1}"
export MPS_SEND_BATCH_DATAGRAMS="${MPS_SEND_BATCH_DATAGRAMS:-32}"
export MPS_KERNEL_PACING_MBPS="${MPS_KERNEL_PACING_MBPS:-20}"

echo "Dual stream: cameras ${left_camera}/${right_camera} -> ${pc_host}:${port}/$((port + 1)) ${eye_width}x${eye_height}@${fps}"
echo "capture=${MPS_CAPTURE_BACKEND} awb=$([[ -n "${MPS_CAMERA_AWB_GAINS:-}" ]] && echo fixed || echo auto) lens=$([[ -n "${MPS_CAMERA_LEFT_LENS_POSITION:-}${MPS_CAMERA_RIGHT_LENS_POSITION:-}" ]] && echo configured || echo auto) exposure=$([[ -n "${MPS_CAMERA_SHUTTER_US:-}" && -n "${MPS_CAMERA_ANALOG_GAIN:-}" ]] && echo fixed || echo auto)"

exec "$sender" "$pc_host" "$port" "$eye_width" "$eye_height" "$fps" \
    "$left_camera" "$right_camera"
