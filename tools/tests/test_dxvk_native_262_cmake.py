import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "android/app/jni/dxvk_native_262/CMakeLists.txt"
JNI_CMAKE = ROOT / "android/app/jni/CMakeLists.txt"


class DxvkNative262CMakeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = CMAKE.read_text(encoding="utf-8")
        cls.root_text = JNI_CMAKE.read_text(encoding="utf-8")

    def test_candidate_is_opt_in_and_keeps_baseline_default(self):
        self.assertIn('option(SNAP_BUILD_DXVK_NATIVE_262', self.root_text)
        self.assertIn('if(SNAP_BUILD_DXVK_NATIVE_262)', self.root_text)
        self.assertIn('add_subdirectory(dxvk_native_262)', self.root_text)
        self.assertRegex(self.text, r'add_library\\(dxvk_d3d9_262 SHARED')
        self.assertIn('DXVK source revision mismatch', self.text)

    def test_android_arm64_soname_contract_is_unversioned(self):
        self.assertIn('OUTPUT_NAME "dxvk_d3d9_262"', self.text)
        self.assertIn('NO_SONAME FALSE', self.text)
        self.assertNotRegex(self.text, r'\\b(?:VERSION|SOVERSION)\\b')

    def test_upstream_source_and_shader_inventory_are_used(self):
        for group in (
            "d3d9 dxvk dxso util spirv vulkan wsi_common android_wsi android_wsi_overlay",
        ):
            self.assertIn(group, self.text)
        self.assertIn("d3d9_shaders dxvk_shaders", self.text)
        self.assertIn("generate_native_262_android_manifest.py", self.text)
        self.assertIn("glslangValidator", self.text)

    def test_android_link_dependencies_fail_fast(self):
        for dependency in (
            'pkg_check_modules(DISPLAY_INFO REQUIRED IMPORTED_TARGET "libdisplay-info<0.2.0")',
            "find_library(SNAP_VULKAN_LIBRARY vulkan REQUIRED)",
            "find_library(SNAP_ANDROID_LIBRARY android REQUIRED)",
            "find_library(SNAP_LOG_LIBRARY log REQUIRED)",
            "SDL2 PkgConfig::DISPLAY_INFO",
        ):
            self.assertIn(dependency, self.text)

    def test_candidate_does_not_replace_production_runtime(self):
        self.assertIn("SNAP_BUILD_DXVK_NATIVE_262", self.root_text)
        self.assertNotIn("SNAP_BUILD_DXVK_NATIVE_262 ON", self.root_text)
        self.assertIn("add_subdirectory(dxvk)", self.root_text)


if __name__ == "__main__":
    unittest.main()
