import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
JAVA = ROOT / "android/app/src/main/java/com/tj/skyrim/SkyrimActivity.java"


class SnapFrame262UiContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = JAVA.read_text(encoding="utf-8")

    def test_ui_does_not_advertise_unimplemented_generated_present(self):
        self.assertIn("Generated frames are not presented yet", self.text)
        self.assertIn("Enable SnapFrame capture / compute (experimental)", self.text)
        self.assertNotIn('frameGeneration.setText("Frame Generation")', self.text)

    def test_ui_only_exposes_supported_262_controls(self):
        self.assertIn('String[] presentModeNames = { "Auto (only supported mode)" };', self.text)
        self.assertIn("presentMode.setEnabled(false);", self.text)
        self.assertIn("Flow Scale: 50% (fixed for the current DXVK 2.6.2 host)", self.text)
        self.assertIn("int scale = 50;", self.text)
        self.assertIn("int mode = 0;", self.text)
        self.assertNotIn("flowScale.getProgress()", self.text)


if __name__ == "__main__":
    unittest.main()
