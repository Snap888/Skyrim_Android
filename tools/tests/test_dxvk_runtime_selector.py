#!/usr/bin/env python3
"""Static safety checks for the Android DXVK runtime selector."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DxvkRuntimeSelectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.selector = (ROOT / "android/app/src/main/java/com/tj/skyrim/DxvkRuntimeSelector.java").read_text(encoding="utf-8")
        cls.launcher = (ROOT / "android/app/src/main/java/com/tj/skyrim/LauncherActivity.java").read_text(encoding="utf-8")
        cls.activity = (ROOT / "android/app/src/main/java/com/tj/skyrim/SkyrimActivity.java").read_text(encoding="utf-8")

    def test_262_is_declared_but_only_exposed_when_both_libraries_are_valid_arm64_elf(self):
        self.assertIn('"native-2.6.2"', self.selector)
        self.assertIn('"dxvk_d3d9_262", "main_262"', self.selector)
        self.assertIn("if (hasPair(dir, NATIVE_262)) variants.add(NATIVE_262);", self.selector)
        self.assertIn('new File(dir, "lib" + variant.dxvkLibrary + ".so")', self.selector)
        self.assertIn('new File(dir, "lib" + variant.mainLibrary + ".so")', self.selector)
        self.assertIn("(header[18] & 0xff) == 0xb7", self.selector)

    def test_full_apk_build_always_packages_the_262_runtime_pair(self):
        workflow = (ROOT / ".github/workflows/android-ci.yml").read_text(encoding="utf-8")
        self.assertIn('-PsnapBuildDxvk262RuntimePair="true"', workflow)
        self.assertIn('BUILD_RUNTIME_PAIR: "true"', workflow)
        self.assertIn('echo "DXVK_NATIVE_262_RUNTIME_PAIR_ABI_OK"', workflow)

    def test_launcher_persists_stable_runtime_id(self):
        self.assertIn("DxvkRuntimeSelector.labels(this)", self.launcher)
        self.assertIn("DxvkRuntimeSelector.idAt(this, dxvkSpinner.getSelectedItemPosition())", self.launcher)

    def test_sdl_loads_selected_pair_before_game_runtime(self):
        self.assertIn("DxvkRuntimeSelector.resolve(this)", self.activity)
        self.assertIn('runtime.dxvkLibrary', self.activity)
        self.assertIn('runtime.mainLibrary', self.activity)


if __name__ == "__main__":
    unittest.main()
