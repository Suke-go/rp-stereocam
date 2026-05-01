#!/usr/bin/env bash
set -euo pipefail

if ! command -v gst-inspect-1.0 >/dev/null 2>&1; then
  echo "gst-inspect-1.0 not found. Run ./tools/pi/bootstrap_pi.sh first." >&2
  exit 1
fi

echo "GStreamer version:"
gst-inspect-1.0 --version

echo
echo "H.264 encoder availability:"
if gst-inspect-1.0 v4l2h264enc >/dev/null 2>&1; then
  echo "v4l2h264enc: available"
else
  echo "v4l2h264enc: not available"
fi

if gst-inspect-1.0 x264enc >/dev/null 2>&1; then
  echo "x264enc    : available"
else
  echo "x264enc    : not available"
fi

echo
echo "H.264 parser:"
gst-inspect-1.0 h264parse >/dev/null
echo "h264parse  : available"
