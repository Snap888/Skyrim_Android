import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = ROOT / "android/app/jni/dxvk_native_262/CMakeLists.txt"
JNI_CMAKE = ROOT / "android/app/jni/CMakeLists.txt"
GAME_CMAKE = ROOT / "android/app/jni/game/CMakeLists.txt"
SNAPFRAME_CMAKE = ROOT / "android/app/jni/snapframe/CMakeLists.txt"
SELECTOR = ROOT / "android/app/src/main/java/com/tj/skyrim/DxvkRuntimeSelector.java"
BUILD_SCRIPT = ROOT / "tools/dxvk/build_native_262_android_candidate.sh"
PATCHER = ROOT / "tools/dxvk/apply_native_262_android_port.py"
DXVK262_API = ROOT / "android/app/jni/snapframe/snapframe_dxvk262_api.h"


class DxvkNative262CMakeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = CMAKE.read_text(encoding="utf-8")
        cls.root_text = JNI_CMAKE.read_text(encoding="utf-8")
        cls.game_text = GAME_CMAKE.read_text(encoding="utf-8")
        cls.snapframe_text = SNAPFRAME_CMAKE.read_text(encoding="utf-8")
        cls.selector_text = SELECTOR.read_text(encoding="utf-8")
        cls.build_script = BUILD_SCRIPT.read_text(encoding="utf-8")
        cls.patcher = PATCHER.read_text(encoding="utf-8")
        cls.dxvk262_api = DXVK262_API.read_text(encoding="utf-8")

    def test_candidate_is_opt_in_and_keeps_baseline_default(self):
        self.assertIn("option(SNAP_BUILD_DXVK_NATIVE_262", self.root_text)
        self.assertIn("if(SNAP_BUILD_DXVK_NATIVE_262)", self.root_text)
        self.assertIn("add_subdirectory(dxvk_native_262)", self.root_text)
        self.assertRegex(self.text, r"add_library\(dxvk_d3d9_262 SHARED")
        self.assertIn("DXVK source revision mismatch", self.text)
        self.assertIn("initialize upstream submodules", self.text)
        self.assertIn("include/native/directx/d3d9.h", self.text)
        self.assertIn("include/vulkan/include/vulkan/vulkan.h", self.text)
        self.assertNotIn("target_link_libraries(dxvk_d3d9_262 PRIVATE\n    SDL2", self.text)

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
            "POSITION_INDEPENDENT_CODE ON",
            "gen-search-table.py",
            "find_package(Threads REQUIRED)",
            "find_library(SNAP_VULKAN_LIBRARY vulkan REQUIRED)",
            "find_library(SNAP_ANDROID_LIBRARY android REQUIRED)",
            "find_library(SNAP_LOG_LIBRARY log REQUIRED)",
            "snap_display_info Threads::Threads",
            "$<TARGET_PROPERTY:SDL2,INTERFACE_INCLUDE_DIRECTORIES>",
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
        self.assertIn('"libSDL2.so"', self.patcher)

    def test_candidate_does_not_replace_production_runtime(self):
        self.assertIn("SNAP_BUILD_DXVK_NATIVE_262", self.root_text)
        self.assertNotIn("SNAP_BUILD_DXVK_NATIVE_262 ON", self.root_text)
        self.assertIn("add_subdirectory(dxvk)", self.root_text)

    def test_runtime_selector_requires_a_complete_arm64_pair(self):
        self.assertIn('"native-1.9.2b"', self.selector_text)
        self.assertIn('"native-2.6.2"', self.selector_text)
        self.assertIn('if (hasPair(dir, NATIVE_262))', self.selector_text)
        self.assertIn('return isArm64Elf(new File(dir, "lib" + variant.dxvkLibrary + ".so"))', self.selector_text)
        self.assertIn('&& isArm64Elf(new File(dir, "lib" + variant.mainLibrary + ".so"))', self.selector_text)
        self.assertIn('"dxvk_d3d9_262", "main_262"', self.selector_text)

    def test_262_present_hook_is_versioned_optional_and_command_stream_local(self):
        # The 2.6.2 hook receives opaque objects from the exact pinned DXVK
        # build and is called before the source WSI blit in the same context.
        # It must never make the legacy host appear compatible with 2.6.2.
        self.assertIn("SNAPFRAME_DXVK262_ABI_VERSION 1u", self.dxvk262_api)
        self.assertIn("void* context", self.dxvk262_api)
        self.assertIn("void* context_objects", self.dxvk262_api)
        self.assertIn("void* device", self.dxvk262_api)
        self.assertIn("void* source_view", self.dxvk262_api)
        self.assertIn("void* window_token", self.dxvk262_api)
        self.assertIn("static_cast<void*>(cPresenter.ptr())", self.patcher)
        self.assertIn("snapframe_dxvk262_release_window", self.patcher)
        self.assertIn("snapframe_dxvk262_release_window(void* window_token)", self.dxvk262_api)
        self.assertIn("snapframe_dxvk262_release_device(void* device_token)", self.dxvk262_api)
        self.assertIn('shutil.copyfile(snapframe_api, dxvk_dir / "snapframe_dxvk262_api.h")', self.patcher)
        self.assertIn("snapframe_dxvk262_release_device(static_cast<void*>(this));", self.patcher)
        self.assertIn("m_objects.pipelineManager().stopWorkerThreads();", self.patcher)
        self.assertIn('device_text.rfind("this->waitForIdle();", 0, stop_offset)', self.patcher)
        self.assertIn("DXVK device teardown no longer drains submissions", self.patcher)
        # Device resources may be retired only after DXVK stops its pipeline
        # workers. Assert the exact replacement block, not loose token presence.
        self.assertIn(
            '"""    m_objects.pipelineManager().stopWorkerThreads();\\n\\n    if (snapframe_dxvk262_release_device)\\n      snapframe_dxvk262_release_device(static_cast<void*>(this));\\n  }\\n"""',
            self.patcher,
        )

        self.assertIn(
            "if (snapframe_dxvk262_release_window)\n          snapframe_dxvk262_release_window(static_cast<void*>(entry->second.presenter.ptr()));\n        entry->second.presenter->destroyResources();",
            self.patcher,
        )
        self.assertIn("__attribute__((weak))", self.dxvk262_api)
        self.assertIn("must not submit/flush it", self.dxvk262_api)
        self.assertIn('snapframe_api = project / "android/app/jni/snapframe/snapframe_dxvk262_api.h"', self.patcher)
        self.assertIn("if (snapframe_dxvk262_record_source)", self.patcher)
        self.assertIn("SNAPFRAME_DXVK262_ABI_VERSION", self.patcher)
        self.assertIn("cSrcView.ptr()", self.patcher)
        replacement_block = self.patcher.split(
            "        // Record optional SnapFrame source work", 1
        )[1].split('        """', 1)[0]
        self.assertLess(
            replacement_block.index("auto contextObjects = ctx->beginExternalRendering()"),
            replacement_block.index("snapframe_dxvk262_record_source("),
        )
        self.assertIn("static_cast<void*>(&contextObjects)", replacement_block)
        self.assertIn("Begin external rendering exactly once", replacement_block)
        self.assertNotIn("ctx->beginExternalRendering()", replacement_block.split(
            "auto contextObjects = ctx->beginExternalRendering()", 1
        )[1])
        self.assertNotIn("snapframe_host_262", self.game_text + self.snapframe_text)

    def test_legacy_snapframe_host_is_not_linked_to_262_candidate(self):
        # The current SnapFrame host consumes DXVK-internal C++ types from the
        # 1.9.2b tree. Keep the candidate isolated until a 2.6.2-native host
        # target compiles against the pinned upstream headers.
        self.assertIn("target_link_libraries(main PRIVATE SDL2 dxvk_d3d9 snapframe_host", self.game_text)
        self.assertIn("target_link_libraries(snapframe_host PUBLIC dxvk_d3d9", self.snapframe_text)
        self.assertNotIn("snapframe_host_262", self.game_text + self.snapframe_text)
        self.assertNotIn("dxvk_d3d9_262 snapframe_host", self.game_text + self.snapframe_text)


if __name__ == "__main__":
    unittest.main()
