#!/usr/bin/env bash
# Capture N synchronized still-image pairs from camera 0 / camera 1 for
# OpenCV stereo calibration (cv2.calibrateCamera + cv2.stereoCalibrate).
#
# Print a checkerboard pattern (e.g. 9x6 internal corners, opencv.org has
# printable ones) on a flat rigid surface. Run this script, and between each
# pair move/tilt the board to a new position+angle within both cameras'
# view -- corners, edges, tilted toward/away from the rig, different
# distances. 20-30 pairs is a reasonable starting point; more pairs with
# more varied poses gives a better calibration, especially at the image
# edges where lens distortion is worst.
#
# Usage:
#   tools/capture_calibration_pairs.sh [out_dir] [count]
#
# Output: <out_dir>/pair_XX_cam0.jpg, <out_dir>/pair_XX_cam1.jpg
# Copy the whole out_dir back to the PC (scp) and run tools/stereo_calibrate.py
# on it there.

set -euo pipefail

camera_cmd=""
if command -v rpicam-jpeg >/dev/null 2>&1; then
  camera_cmd="rpicam-jpeg"
elif command -v libcamera-jpeg >/dev/null 2>&1; then
  camera_cmd="libcamera-jpeg"
else
  echo "rpicam-jpeg/libcamera-jpeg not found" >&2
  exit 1
fi

out_dir="${1:-$HOME/metapuppet_calibration}"
count="${2:-25}"
mkdir -p "${out_dir}"

echo "Will capture ${count} pairs into ${out_dir}"
echo "Move the checkerboard to a new position/angle before each pair."
echo ""

for i in $(seq -w 1 "${count}"); do
  read -r -p "[pair ${i}/${count}] position the board, then press Enter to capture (or 's' to skip, 'q' to stop early): " ans
  if [ "${ans}" = "q" ]; then
    echo "Stopping early at pair ${i}."
    break
  fi
  if [ "${ans}" = "s" ]; then
    echo "Skipped pair ${i}."
    continue
  fi

  cam0_path="${out_dir}/pair_${i}_cam0.jpg"
  cam1_path="${out_dir}/pair_${i}_cam1.jpg"

  # Back-to-back stills, not truly simultaneous -- fine for a rigid board
  # the operator holds still for a second between the two shots.
  "${camera_cmd}" --camera 0 -o "${cam0_path}" --timeout 500 --nopreview
  "${camera_cmd}" --camera 1 -o "${cam1_path}" --timeout 500 --nopreview

  echo "  saved ${cam0_path} / ${cam1_path}"
done

echo ""
echo "Done. Copy this directory to the PC and run:"
echo "  python tools/stereo_calibrate.py ${out_dir}"
