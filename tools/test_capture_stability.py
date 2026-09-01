from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CaptureStabilityContract(unittest.TestCase):
    def test_dual_launcher_uses_exported_opt_in_camera_config(self):
        launcher = (ROOT / "tools/run_stereo_dual.sh").read_text()
        self.assertIn('config_file="${MPS_CONFIG_FILE:-', launcher)
        self.assertIn("set -a", launcher)
        self.assertNotIn("MPS_USE_CALIBRATION_EXPOSURE", launcher)
        self.assertNotIn("MPS_CAMERA_AWB_GAINS:-1.769247", launcher)
        self.assertIn('MPS_CAMERA_AWB_GAINS:-}', launcher)
        self.assertIn('MPS_CAMERA_SHUTTER_US:-}', launcher)
        self.assertIn('MPS_CAMERA_ANALOG_GAIN:-}', launcher)

        config = (ROOT / "tools/metapuppet_pi.env").read_text()
        for empty_setting in (
            "MPS_CAMERA_AWB_GAINS=",
            "MPS_CAMERA_SHUTTER_US=",
            "MPS_CAMERA_ANALOG_GAIN=",
            "MPS_CAMERA_LEFT_LENS_POSITION=",
            "MPS_CAMERA_RIGHT_LENS_POSITION=",
        ):
            self.assertIn(empty_setting, config)

    def test_v4l2_enhanced_controls_have_finite_fallbacks(self):
        source = (
            ROOT / "native/stream_sender/src/mps_libcamera_sbs_sender.cpp"
        ).read_text()
        self.assertIn('environmentSwitch("MPS_V4L2_CBR", true)', source)
        self.assertIn("video_bitrate_mode=1", source)
        self.assertIn("h264_intra_refresh_period", source)
        self.assertIn("retrying the legacy bitrate-only hardware pipeline", source)
        self.assertIn("falling back to x264enc", source)
        self.assertLess(
            source.index("retrying the legacy bitrate-only hardware pipeline"),
            source.index("falling back to x264enc"),
        )

    def test_deploy_does_not_overwrite_device_capture_config(self):
        deploy = (ROOT / "tools/deploy_stereo_dual.ps1").read_text()
        self.assertIn("metapuppet_pi.env.example", deploy)
        self.assertNotIn(
            '"$($target):$RemotePath/tools/metapuppet_pi.env"', deploy
        )


if __name__ == "__main__":
    unittest.main()
