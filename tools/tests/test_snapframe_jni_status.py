import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN_C = ROOT / "runtime/main.c"


class SnapFrameJniStatusContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = MAIN_C.read_text(encoding="utf-8")

    def test_get_settings_treats_zero_as_success(self):
        self.assertIn("int settings_status = snapframe_host_get_settings(&enabled, &flow_percent);", self.text)
        self.assertIn("int active = settings_status == 0 ? 1 : 0;", self.text)
        self.assertIn("flow_percent = 50", self.text)

    def test_set_settings_does_not_invert_errno_style_status(self):
        self.assertIn("snapframe_host_update_settings(enabled ? 1 : 0, flow_percent) != 0", self.text)
        self.assertIn("snapframe_host_update_present_mode(present_mode) != 0", self.text)
        self.assertNotIn("if (!snapframe_host_update_settings", self.text)
        self.assertNotIn("|| !snapframe_host_update_present_mode", self.text)

    def test_present_mode_falls_back_to_auto_when_api_is_unavailable(self):
        self.assertIn("if (snapframe_host_get_present_mode(&present_mode) != 0)", self.text)
        self.assertIn("present_mode = 0", self.text)


if __name__ == "__main__":
    unittest.main()
