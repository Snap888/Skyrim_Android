import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "android/app/jni/dxvk_native_262/CMakeLists.txt"
JNI_CMAKE = ROOT / "android/app/jni/CMakeLists.txt"
BUILD_SCRIPT = ROOT / "tools/dxvk/build_native_262_android_candidate.sh"


class DxvkNative262CMakeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = CMAKE.read_text(encoding="utf-8")
        cls.root_text = JNI_CMAKE.read_text(encoding="utf-8")
        cls.build_script = BUILD_SCRIPT.read_text(encoding="utf-8")

    def test_candidate_is_opt_in_and_keeps_baseline_default(self):
        self.assertIn("option(SNAP_BUILD_DXVK_NATIVE_262", self.root_text)
        self.assertIn("if(SNAP_BUILD_DXVK_NATIVE_262)", self.root_text)
        self.assertIn("add_subdirectory(dxvk_native_262)", self.root_text)
        self.assertRegex(self.text, r"add_library\(dxvk_d3d9_262 SHARED")
        self.assertIn("DXVK source revision mismatch", self.text)

    def test_android_arm64_soname_contract_is_unversioned(self):
        self.assertIn('OUTPUT_NAME "dxvk_d3d9_262"', self.text)
        self.assertIn("NO_SONAME FALSE", self.text)
        self.assertNotRegex(self.text, r"\b(?:VERSION|SOVERSION)\b")

    def test_upstream_source_and_shader_inventory_are_used(self):
        self.assertIn(
            "d3d9 dxvk dxso util spirv vulkan wsi_common android_wsi android_wsi_overlay",
            self.text,
        )
        self.assertIn("d3d9_shaders dxvk_shaders", self.text)
        self.assertIn("generate_native_262_android_manifest.py", self.text)
        self.assertIn("glslangValidator", self.text)
        self.assertIn("--depfile", self.text)
        self.assertIn('DEPFILE "${_header}.d"', self.text)

    def test_android_link_dependencies_fail_fast(self):
        for dependency in (
            "GIT_TAG 92b031749c0fe84ef5cdf895067b84a829920e25",
            "snap_display_info STATIC",
            "gen-search-table.py",
            "find_package(Threads REQUIRED)",
            "find_library(SNAP_VULKAN_LIBRARY vulkan REQUIRED)",
            "find_library(SNAP_ANDROID_LIBRARY android REQUIRED)",
            "find_library(SNAP_LOG_LIBRARY log REQUIRED)",
            "SDL2 snap_display_info Threads::Threads",
        ):
            self.assertIn(dependency, self.text)


    def test_arm64_build_script_checks_elf_soname_and_exports(self):
        self.assertIn("android.toolchain.cmake", self.build_script)
        self.assertIn('ABI=${ANDROID_ABI:-arm64-v8a}', self.build_script)
        self.assertIn("llvm-readelf", self.build_script)
        self.assertIn("llvm-nm", self.build_script)
        self.assertIn("Library soname: [libdxvk_d3d9_262.so]", self.build_script)
        self.assertIn("Direct3DCreate9", self.build_script)
        self.assertIn("Direct3DCreate9Ex", self.build_script)

    def test_candidate_does_not_replace_production_runtime(self):
        self.assertIn("SNAP_BUILD_DXVK_NATIVE_262", self.root_text)
        self.assertNotIn("SNAP_BUILD_DXVK_NATIVE_262 ON", self.root_text)
        self.assertIn("add_subdirectory(dxvk)", self.root_text)


if __name__ == "__main__":
    unittest.main()
