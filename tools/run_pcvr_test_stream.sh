#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../.." && pwd)"
config_file="${script_dir}/metapuppet_pi.env"

if [[ -f "${config_file}" ]]; then
  # shellcheck disable=SC1090
  source "${config_file}"
fi

host="${1:-${METAPUPPET_PC_HOST:-192.168.50.1}}"
port="${2:-${METAPUPPET_PC_PORT:-5004}}"
width="${3:-1280}"
height="${4:-360}"
fps="${5:-30}"

sender="${repo_root}/build/pi_stream_sender/mps_pcvr_test_sender"
if [[ ! -x "${sender}" ]]; then
  echo "test sender not built. Running build first." >&2
  "${script_dir}/build_pi_streaming_stack.sh"
fi

echo "Raw RGBA SBS test stream"
echo "target : ${host}:${port}"
echo "format : ${width}x${height}@${fps}"
echo "note   : Raw RGBA is only for Ethernet/link validation; encoded NV12/H.264 follows in the capture path."

exec "${sender}" "${host}" "${port}" "${width}" "${height}" "${fps}"
