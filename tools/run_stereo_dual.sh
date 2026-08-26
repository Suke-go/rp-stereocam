#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sender="${repo_root}/build/pi_stream_sender/mps_stereo_x264_sender"

pc_host="${1:-192.168.50.1}"
port="${2:-5004}"
eye_width="${3:-1280}"
eye_height="${4:-720}"
fps="${5:-60}"
left_camera="${6:-0}"
right_camera="${7:-1}"

if [[ ! -x "$sender" ]]; then
    echo "Dual sender is not built: $sender" >&2
    echo "Run tools/deploy_stereo_dual.ps1 from Windows or build mps_stereo_x264_sender on this Pi." >&2
    exit 1
fi

# s1 calibration: preserve colour and rectification geometry at both 30/60 fps.
# Exposure stays automatic by default because the calibrated 26974 us shutter
# cannot meet a 60 fps (16667 us) frame interval.
export MPS_CAPTURE_BACKEND="${MPS_CAPTURE_BACKEND:-libcamera}"
export MPS_CAMERA_AWB_GAINS="${MPS_CAMERA_AWB_GAINS:-1.769247,2.343446}"
export MPS_CAMERA_LEFT_LENS_POSITION="${MPS_CAMERA_LEFT_LENS_POSITION:-0.792}"
export MPS_CAMERA_RIGHT_LENS_POSITION="${MPS_CAMERA_RIGHT_LENS_POSITION:-0.787}"

if [[ "${MPS_USE_CALIBRATION_EXPOSURE:-0}" == "1" ]]; then
    export MPS_CAMERA_SHUTTER_US="${MPS_CAMERA_SHUTTER_US:-26974}"
    export MPS_CAMERA_ANALOG_GAIN="${MPS_CAMERA_ANALOG_GAIN:-1.499268}"
    if (( fps > 30 )); then
        echo "warning: ${MPS_CAMERA_SHUTTER_US} us shutter cannot sustain ${fps} fps; use 30 fps or leave MPS_USE_CALIBRATION_EXPOSURE=0" >&2
    fi
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
echo "capture=${MPS_CAPTURE_BACKEND} awb=fixed lens=fixed exposure=$([[ "${MPS_USE_CALIBRATION_EXPOSURE:-0}" == "1" ]] && echo fixed || echo auto)"

exec "$sender" "$pc_host" "$port" "$eye_width" "$eye_height" "$fps" \
    "$left_camera" "$right_camera"
