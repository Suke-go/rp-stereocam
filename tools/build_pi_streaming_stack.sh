#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
  echo "cmake/ninja not found. Run ./tools/pi/bootstrap_pi.sh first." >&2
  exit 1
fi

cmake -S "${repo_root}/native/edge_camera" \
  -B "${repo_root}/build/pi_edge_camera" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMP_EDGE_ENABLE_NEON=ON
cmake --build "${repo_root}/build/pi_edge_camera"

cmake -S "${repo_root}/native/stream_sender" \
  -B "${repo_root}/build/pi_stream_sender" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${repo_root}/build/pi_stream_sender"

"${repo_root}/build/pi_edge_camera/mp_edge_camera"

echo "Pi streaming stack build complete"
echo "test sender: ${repo_root}/build/pi_stream_sender/mps_pcvr_test_sender"
echo "camera sender: ${repo_root}/build/pi_stream_sender/mps_libcamera_sbs_sender"
