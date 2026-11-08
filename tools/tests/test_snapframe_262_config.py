import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "runtime/snapframe_bootstrap.c"
RUNTIME = ROOT / "android/app/jni/snapframe/snapframe_runtime_262.cpp"


class SnapFrame262ConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
        cls.runtime = RUNTIME.read_text(encoding="utf-8")

    def test_262_bootstrap_normalizes_unavailable_adaptive_mode(self):
        guard = "#if defined(SNAP_DXVK_NATIVE_262_RUNTIME)"
        self.assertIn(guard, self.bootstrap)
        block = self.bootstrap.split(guard, 1)[1].split("#endif", 1)[0]
        self.assertIn("config.ai_mode = SNAPFRAME_AI_MANUAL;", block)
        self.assertIn("adaptive telemetry is not connected yet", block)
        self.assertLess(
            self.bootstrap.index("config.ai_mode = SNAPFRAME_AI_MANUAL;"),
            self.bootstrap.index("snapframe_host_init(&config);"),
        )

    def test_262_generated_wsi_gate_keeps_compute_only_path_disabled(self):
        self.assertIn("constexpr bool kGeneratedWsiPresentationReady = false;", self.runtime)
        self.assertIn("return kGeneratedWsiPresentationReady", self.runtime)
        self.assertIn("generated WSI present is not ready; skipping capture/compute", self.runtime)
        self.assertIn("c->ai_mode == SNAPFRAME_AI_MANUAL", self.runtime)
        self.assertIn("c->flow_scale_percent == kSupportedFlowScale", self.runtime)

    def test_successful_zero_status_does_not_emit_bootstrap_warning(self):
        self.assertIn("if (initResult < 0 || (config.enabled && !activeAfterInit))", self.bootstrap)
        self.assertNotIn("if (initResult <= 0 ||", self.bootstrap)

    def test_legacy_runtime_keeps_user_ai_mode(self):
        self.assertIn("#if defined(SNAP_DXVK_NATIVE_262_RUNTIME)", self.bootstrap)
        self.assertIn("legacy DXVK keeps the user's configured AI mode", self.bootstrap)


if __name__ == "__main__":
    unittest.main()
