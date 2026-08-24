#!/usr/bin/env python3
"""Stereo camera calibration for the MetaPuppet DIY Pi rig.

Diagnoses and quantifies the roll/pitch/yaw misalignment between the two
Pi camera modules (the likely cause of "double vision, doesn't fuse"
stereo symptoms with no code bug involved -- see rp-stereocam/docs), and
produces the data needed to correct for it.

Input: a directory of pair_NN_cam0.jpg / pair_NN_cam1.jpg checkerboard
photos from tools/capture_calibration_pairs.sh (run on the Pi, copied
back to this machine).

Output (written into <input_dir>/calibration_result/):
  - calibration.json   camera matrices, distortion coeffs, R, T, and the
                        decomposed roll/pitch/yaw between the two cameras
                        in degrees -- this is the number that answers
                        "how bad is the misalignment".
  - rectify_maps.npz   full stereoRectify undistort+rectify maps (for a
                        later full-quality remap-based correction, e.g.
                        baked into the receiver or the Unity shader).
  - rectified_pair_00.jpg, ... a few sample rectified pairs with
                        horizontal guide lines drawn across both eyes --
                        if the same real-world point sits on the same
                        guide line in both images, the correction worked.

Usage:
    python tools/stereo_calibrate.py <calibration_dir> [--board-cols 9]
        [--board-rows 6] [--square-size-mm 25.0]

Requires: opencv-python, numpy (pip install opencv-python numpy)
"""

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np


def find_pairs(calibration_dir: Path):
    cam0_paths = sorted(calibration_dir.glob("pair_*_cam0.jpg"))
    pairs = []
    for cam0_path in cam0_paths:
        cam1_path = calibration_dir / cam0_path.name.replace("_cam0.", "_cam1.")
        if cam1_path.exists():
            pairs.append((cam0_path, cam1_path))
        else:
            print(f"  [skip] no matching cam1 for {cam0_path.name}")
    return pairs


def detect_corners(pairs, board_size):
    """Returns (image_size, objpoints, imgpoints0, imgpoints1, used_pairs)."""
    objp = np.zeros((board_size[0] * board_size[1], 3), np.float32)
    objp[:, :2] = np.mgrid[0 : board_size[0], 0 : board_size[1]].T.reshape(-1, 2)

    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-5)
    objpoints = []
    imgpoints0 = []
    imgpoints1 = []
    used_pairs = []
    image_size = None

    for cam0_path, cam1_path in pairs:
        img0 = cv2.imread(str(cam0_path))
        img1 = cv2.imread(str(cam1_path))
        if img0 is None or img1 is None:
            print(f"  [skip] failed to read {cam0_path.name}/{cam1_path.name}")
            continue
        gray0 = cv2.cvtColor(img0, cv2.COLOR_BGR2GRAY)
        gray1 = cv2.cvtColor(img1, cv2.COLOR_BGR2GRAY)
        if image_size is None:
            image_size = (gray0.shape[1], gray0.shape[0])

        found0, corners0 = cv2.findChessboardCorners(gray0, board_size)
        found1, corners1 = cv2.findChessboardCorners(gray1, board_size)
        if not (found0 and found1):
            print(f"  [skip] checkerboard not found in {cam0_path.name} (cam0={found0}, cam1={found1})")
            continue

        corners0 = cv2.cornerSubPix(gray0, corners0, (11, 11), (-1, -1), criteria)
        corners1 = cv2.cornerSubPix(gray1, corners1, (11, 11), (-1, -1), criteria)

        objpoints.append(objp)
        imgpoints0.append(corners0)
        imgpoints1.append(corners1)
        used_pairs.append((cam0_path, cam1_path))
        print(f"  [ok] {cam0_path.name}")

    return image_size, objpoints, imgpoints0, imgpoints1, used_pairs


def rotation_matrix_to_euler_deg(R):
    """Decompose a rotation matrix into roll/pitch/yaw in degrees.

    Roll = rotation about the optical (Z) axis -- this is the one that
    produces the "image tilted at an angle" / diagonal-disparity symptom
    the user described. Pitch/yaw are vertical/horizontal aim mismatch.
    """
    sy = np.sqrt(R[0, 0] ** 2 + R[1, 0] ** 2)
    singular = sy < 1e-6
    if not singular:
        pitch = np.arctan2(-R[2, 0], sy)
        yaw = np.arctan2(R[1, 0], R[0, 0])
        roll = np.arctan2(R[2, 1], R[2, 2])
    else:
        pitch = np.arctan2(-R[2, 0], sy)
        yaw = 0.0
        roll = np.arctan2(-R[1, 2], R[1, 1])
    return np.degrees(roll), np.degrees(pitch), np.degrees(yaw)


def draw_guide_lines(img, spacing=40, color=(0, 255, 0)):
    out = img.copy()
    for y in range(0, out.shape[0], spacing):
        cv2.line(out, (0, y), (out.shape[1], y), color, 1)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("calibration_dir", type=Path, help="Directory with pair_NN_cam0.jpg / pair_NN_cam1.jpg")
    parser.add_argument("--board-cols", type=int, default=9, help="Internal corners along the checkerboard's long side")
    parser.add_argument("--board-rows", type=int, default=6, help="Internal corners along the checkerboard's short side")
    parser.add_argument("--square-size-mm", type=float, default=25.0, help="Physical size of one checkerboard square, mm")
    args = parser.parse_args()

    if not args.calibration_dir.is_dir():
        print(f"Not a directory: {args.calibration_dir}", file=sys.stderr)
        return 1

    board_size = (args.board_cols, args.board_rows)
    pairs = find_pairs(args.calibration_dir)
    if not pairs:
        print(f"No pair_*_cam0.jpg / pair_*_cam1.jpg found in {args.calibration_dir}", file=sys.stderr)
        return 1
    print(f"Found {len(pairs)} candidate pairs. Detecting checkerboard corners...")

    image_size, objpoints, imgpoints0, imgpoints1, used_pairs = detect_corners(pairs, board_size)
    if len(used_pairs) < 8:
        print(f"\nOnly {len(used_pairs)} usable pairs (need >= 8, ideally 15-25+ with varied poses).")
        print("Re-run tools/capture_calibration_pairs.sh with more/varied checkerboard positions.")
        return 1
    print(f"\nUsing {len(used_pairs)}/{len(pairs)} pairs for calibration.")

    # Scale object points to real-world units (mm) so T (translation / the
    # baseline) comes out in physically meaningful units.
    objpoints = [op * args.square_size_mm for op in objpoints]

    print("Calibrating camera 0...")
    rms0, mtx0, dist0, _, _ = cv2.calibrateCamera(objpoints, imgpoints0, image_size, None, None)
    print(f"  cam0 reprojection RMS error: {rms0:.3f}px")

    print("Calibrating camera 1...")
    rms1, mtx1, dist1, _, _ = cv2.calibrateCamera(objpoints, imgpoints1, image_size, None, None)
    print(f"  cam1 reprojection RMS error: {rms1:.3f}px")

    print("Running stereoCalibrate (solving for relative rotation/translation)...")
    flags = cv2.CALIB_FIX_INTRINSIC
    criteria = (cv2.TERM_CRITERIA_EPS + cv2.TERM_CRITERIA_MAX_ITER, 100, 1e-5)
    rms_stereo, mtx0, dist0, mtx1, dist1, R, T, E, F = cv2.stereoCalibrate(
        objpoints, imgpoints0, imgpoints1, mtx0, dist0, mtx1, dist1, image_size,
        criteria=criteria, flags=flags,
    )
    print(f"  stereo reprojection RMS error: {rms_stereo:.3f}px")

    roll_deg, pitch_deg, yaw_deg = rotation_matrix_to_euler_deg(R)
    baseline_mm = float(np.linalg.norm(T))

    print("\n=== Misalignment between camera 0 and camera 1 ===")
    print(f"  Roll  (image-plane tilt, matches 'diagonal double vision'): {roll_deg:+.2f} deg")
    print(f"  Pitch (vertical aim mismatch):                              {pitch_deg:+.2f} deg")
    print(f"  Yaw   (horizontal aim / convergence):                       {yaw_deg:+.2f} deg")
    print(f"  Baseline (distance between lenses):                        {baseline_mm:.1f} mm")
    print("  (Average human IPD is roughly 54-74mm, ~63mm typical.)")
    if abs(roll_deg) > 1.0:
        print(f"  -> Roll of {roll_deg:+.2f} deg is almost certainly why the image looks tilted/doubled;")
        print("     a few tenths of a degree is already noticeable, a few degrees is unusable uncorrected.")

    print("\nComputing stereoRectify + undistort/rectify maps...")
    R0, R1, P0, P1, Q, roi0, roi1 = cv2.stereoRectify(
        mtx0, dist0, mtx1, dist1, image_size, R, T, alpha=0
    )
    map0x, map0y = cv2.initUndistortRectifyMap(mtx0, dist0, R0, P0, image_size, cv2.CV_32FC1)
    map1x, map1y = cv2.initUndistortRectifyMap(mtx1, dist1, R1, P1, image_size, cv2.CV_32FC1)

    out_dir = args.calibration_dir / "calibration_result"
    out_dir.mkdir(exist_ok=True)

    result = {
        "image_size": image_size,
        "camera_matrix_0": mtx0.tolist(),
        "dist_coeffs_0": dist0.flatten().tolist(),
        "camera_matrix_1": mtx1.tolist(),
        "dist_coeffs_1": dist1.flatten().tolist(),
        "R": R.tolist(),
        "T_mm": T.flatten().tolist(),
        "roll_deg": roll_deg,
        "pitch_deg": pitch_deg,
        "yaw_deg": yaw_deg,
        "baseline_mm": baseline_mm,
        "stereo_reprojection_rms_px": rms_stereo,
        "pairs_used": len(used_pairs),
        "pairs_total": len(pairs),
    }
    (out_dir / "calibration.json").write_text(json.dumps(result, indent=2))
    print(f"Wrote {out_dir / 'calibration.json'}")

    np.savez(out_dir / "rectify_maps.npz", map0x=map0x, map0y=map0y, map1x=map1x, map1y=map1y, Q=Q)
    print(f"Wrote {out_dir / 'rectify_maps.npz'}")

    sample_count = min(3, len(used_pairs))
    for i in range(sample_count):
        cam0_path, cam1_path = used_pairs[i]
        img0 = cv2.imread(str(cam0_path))
        img1 = cv2.imread(str(cam1_path))
        rect0 = cv2.remap(img0, map0x, map0y, cv2.INTER_LINEAR)
        rect1 = cv2.remap(img1, map1x, map1y, cv2.INTER_LINEAR)
        rect0 = draw_guide_lines(rect0)
        rect1 = draw_guide_lines(rect1)
        combined = np.hstack([rect0, rect1])
        out_path = out_dir / f"rectified_pair_{i:02d}.jpg"
        cv2.imwrite(str(out_path), combined)
        print(f"Wrote {out_path} (check that a real-world point lands on the same guide line in both halves)")

    print("\nDone. If roll/pitch/yaw are small enough to live with, apply the correction as a")
    print("software rectification step instead of re-mounting the cameras -- see")
    print("rp-stereocam/docs for how this feeds into the receiver/shader.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
