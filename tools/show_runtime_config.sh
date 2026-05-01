#!/usr/bin/env bash
set -euo pipefail

config_file="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/metapuppet_pi.env"
if [[ -f "${config_file}" ]]; then
  # shellcheck disable=SC1090
  source "${config_file}"
fi

echo "left camera : ${METAPUPPET_LEFT_CAMERA:-1}"
echo "right camera: ${METAPUPPET_RIGHT_CAMERA:-0}"
echo "PC target   : ${METAPUPPET_PC_HOST:-192.168.50.1}:${METAPUPPET_PC_PORT:-5004}"
echo "mode        : ${METAPUPPET_EYE_WIDTH:-1536}x${METAPUPPET_EYE_HEIGHT:-864}@${METAPUPPET_FPS:-60}"
