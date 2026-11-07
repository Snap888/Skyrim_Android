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
PRESENTATION_DESIGN = ROOT / "docs/SNAPFRAME_DXVK262_PRESENTATION_DESIGN.md"


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

    def test_android_patcher_resolves_dxvk_dir_before_using_source_paths(self):
        self.assertLess(
            self.patcher.index('dxvk_dir = root / "src/dxvk"'),
            self.patcher.index('presenter_h = dxvk_dir / "dxvk_presenter.h"'),
        )
        self.assertEqual(self.patcher.count('dxvk_dir = root / "src/dxvk"'), 1)

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

    def test_candidate_build_compiles_the_version_matched_snapframe_host(self):
        self.assertIn("tools/compile_snapframe_shaders.py", self.build_script)
        self.assertIn("--spirv-val", self.build_script)
        self.assertIn("snapframe_host_262.cpp", self.build_script)
        self.assertIn("target_sources(dxvk_d3d9_262 PRIVATE", self.build_script)
        self.assertIn("BUILD_SNAPFRAME_GENERATED_DIR", self.build_script)
        self.assertIn("snapframe_dxvk262_record_source", self.build_script)
        self.assertIn("snapframe_dxvk262_release_window", self.build_script)
        self.assertIn("snapframe_dxvk262_release_device", self.build_script)
        self.assertIn("for symbol in dxvk_native_surface snapframe_dxvk262_record_source snapframe_dxvk262_release_window snapframe_dxvk262_release_device", self.build_script)
        self.assertIn("ELF_DEFINED", self.build_script)
        self.assertIn("ELF_EXPORTS", self.build_script)
        self.assertIn('SYMBOL_MAP="$DXVK_SOURCE_DIR/src/d3d9/d3d9.sym"', self.build_script)
        self.assertIn('"dxvk_native_surface"', self.build_script)
        self.assertIn('for symbol in dxvk_native_surface snapframe_dxvk262_record_source snapframe_dxvk262_release_window snapframe_dxvk262_release_device', self.build_script)
        self.assertIn('<<<"$ELF_EXPORTS"', self.build_script)
        self.assertIn("required_exports = (", self.patcher)
        self.assertIn("DXVK_NATIVE_262_SNAPFRAME_DYNAMIC_EXPORTS_OK", self.patcher)
        self.assertIn('symbol_map = root / "src/d3d9/d3d9.sym"', self.patcher)
        self.assertIn("DXVK_NATIVE_262_SNAPFRAME_HOST_COMPILED_OK", self.build_script)
        self.assertIn("#if defined(_WIN32) && !defined(__ANDROID__)", self.patcher)
        self.assertIn("VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME", self.patcher)
        self.assertIn("VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME", self.patcher)
        self.assertIn("VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME", self.patcher)

    def test_android_loader_keeps_compat_types_without_win32_vulkan_platform(self):
        # HMODULE comes from DXVK's Win32 compatibility layer, not Vulkan's
        # platform declarations. The loader header must include it explicitly.
        self.assertIn('vulkan_loader = root / "src/vulkan/vulkan_loader.h"', self.patcher)
        self.assertIn('#include "../util/util_win32_compat.h"', self.patcher)
        self.assertIn('#if defined(_WIN32) && !defined(__ANDROID__)', self.patcher)
        self.assertNotIn('#define VK_USE_PLATFORM_WIN32_KHR 1\\n', self.patcher)

    def test_presenter_path_is_initialized_before_android_mode_patch(self):
        # The overlay applies the Android present-mode guard before the later
        # WSI transaction block; its source path must already be initialized.
        self.assertLess(
            self.patcher.index('presenter_cpp = dxvk_dir / "dxvk_presenter.cpp"'),
            self.patcher.index("presenter_modes = "),
        )

    def test_android_presenter_uses_portable_present_mode_query(self):
        # Full-screen-exclusive present-mode query is Win32-only. Android
        # must compile only the portable KHR query and never load the EXT fn.
        self.assertIn("presenter_modes = ", self.patcher)
        self.assertIn("#ifdef _WIN32", self.patcher)
        self.assertIn("vkGetPhysicalDeviceSurfacePresentModes2EXT", self.patcher)
        self.assertIn("vkGetPhysicalDeviceSurfacePresentModesKHR", self.patcher)
        self.assertIn("presenter_modes_second", self.patcher)

    def test_android_presenter_reuses_suboptimal_swapchain_instead_of_recreating_every_frame(self):
        # Adreno reports VK_SUBOPTIMAL_KHR persistently for this Android
        # surface. It is a usable swapchain status, not an out-of-date signal.
        self.assertIn("replace_exact(", self.patcher)
        self.assertIn("acquire_status = ", self.patcher)
        self.assertIn("        2,", self.patcher)
        self.assertIn("if (m_acquireStatus == VK_SUBOPTIMAL_KHR)", self.patcher)
        self.assertIn("m_acquireStatus = VK_SUCCESS;", self.patcher)
        self.assertIn("if (status == VK_SUBOPTIMAL_KHR)", self.patcher)
        self.assertIn("status = VK_SUCCESS;", self.patcher)
        self.assertIn("DXVK_NATIVE_262_ANDROID_SUBOPTIMAL_REUSE_OK", self.patcher)
        self.assertIn("reserve recreation for OUT_OF_DATE / SURFACE_LOST", self.patcher)

    def test_android_fence_excludes_win32_semaphore_handle_api(self):
        # DXVK's HANDLE import/export bridge is Win32-only. Android must keep
        # native Vulkan semaphore operation and compile no Win32 Vk structs.
        self.assertIn('fence_cpp = dxvk_dir / "dxvk_fence.cpp"', self.patcher)
        self.assertIn('#ifdef _WIN32', self.patcher)
        self.assertIn('VkImportSemaphoreWin32HandleInfoKHR', self.patcher)
        self.assertIn('VkSemaphoreGetWin32HandleInfoKHR', self.patcher)
        self.assertIn('return INVALID_HANDLE_VALUE;', self.patcher)
        self.assertIn('External Win32 semaphore handles are unavailable on this platform', self.patcher)

    def test_android_image_import_excludes_win32_shared_handle_structs(self):
        # DxvkSharedImageInfo stores a Win32 HANDLE. It is not an Android FD,
        # so Android must compile normal image allocation without this path.
        self.assertIn('image_cpp = dxvk_dir / "dxvk_image.cpp"', self.patcher)
        self.assertIn('VkImportMemoryWin32HandleInfoKHR sharedImportWin32', self.patcher)
        self.assertIn('#ifdef _WIN32', self.patcher)
        self.assertIn('DXVK Win32 shared-image handles are unavailable on Android', self.patcher)
        self.assertIn('sharedMemoryInfo = nullptr;', self.patcher)

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
        self.assertIn("SNAPFRAME_DXVK262_ABI_VERSION 3u", self.dxvk262_api)
        self.assertIn("void* context", self.dxvk262_api)
        self.assertIn("void* context_objects", self.dxvk262_api)
        self.assertIn("void* device", self.dxvk262_api)
        self.assertIn("const void* source_view_ref", self.dxvk262_api)
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
        self.assertIn("Presenter acquire serialization changed; re-review generated-present scheduling", self.patcher)
        self.assertIn("Presenter present queue path changed; re-review generated-present scheduling", self.patcher)
        self.assertIn("Presenter no longer pre-acquires after present; re-review slot ownership", self.patcher)
        self.assertIn("Presenter semaphore ring changed; re-review generated-present scheduling", self.patcher)
        self.assertIn("Presenter next-slot fence reuse changed; re-review generated-present scheduling", self.patcher)
        self.assertIn("SnapFrame generated present: DISABLED pending a DXVK-owned asynchronous transaction scheduler.", self.patcher)
        # Device teardown callback is part of the patcher's guarded replacement.
        self.assertIn("snapframe_dxvk262_release_device(static_cast<void*>(this));", self.patcher)

        self.assertIn(
            "if (snapframe_dxvk262_release_window)\n          snapframe_dxvk262_release_window(static_cast<void*>(entry->second.presenter.ptr()));\n        entry->second.presenter->destroyResources();",
            self.patcher,
        )
        self.assertIn("__attribute__((weak))", self.dxvk262_api)
        self.assertIn("must not submit/flush it", self.dxvk262_api)
        self.assertIn('snapframe_api = project / "android/app/jni/snapframe/snapframe_dxvk262_api.h"', self.patcher)
        self.assertIn("if (snapframe_dxvk262_record_source)", self.patcher)
        self.assertIn("SNAPFRAME_DXVK262_ABI_VERSION", self.patcher)
        self.assertIn("static_cast<const void*>(&cSrcView)", self.patcher)
        replacement_block = self.patcher.split(
            "        // Begin external rendering exactly once.", 1
        )[1].split('        """', 1)[0]
        self.assertLess(
            replacement_block.index("auto contextObjects = ctx->beginExternalRendering()"),
            replacement_block.index("snapframe_dxvk262_record_source("),
        )
        self.assertIn("static_cast<void*>(&contextObjects)", replacement_block)
        self.assertNotIn("ctx->beginExternalRendering()", replacement_block.split(
            "auto contextObjects = ctx->beginExternalRendering()", 1
        )[1])
        self.assertIn("snapframe_host_262.cpp", self.snapframe_text)
        self.assertIn("target_sources(dxvk_d3d9_262 PRIVATE", self.snapframe_text)
        self.assertIn("SnapFrame DXVK 2.6.2 compute host + explicit runtime ABI enabled", self.snapframe_text)
        self.assertNotIn("snapframe_host_262", self.game_text)

    def test_262_runtime_enable_api_has_consistent_c_linkage_and_fails_closed(self):
        # These declarations are included by both C++ implementation and
        # patched DXVK translation units. Keep them inside the extern-C block
        # so the exported ABI has unmangled names on Android.
        extern_open = self.dxvk262_api.index('extern "C" {')
        extern_close = self.dxvk262_api.index("#ifdef __cplusplus\n}", extern_open)
        for symbol in (
            "snapframe_dxvk262_set_enabled",
            "snapframe_dxvk262_get_enabled",
        ):
            declaration = self.dxvk262_api.index(symbol)
            self.assertGreater(declaration, extern_open)
            self.assertLess(declaration, extern_close)
        host = (ROOT / "android/app/jni/snapframe/snapframe_host_262.cpp").read_text(encoding="utf-8")
        self.assertIn("std::atomic<bool> g_enabled{false};", host)
        self.assertIn("g_enabled.load(std::memory_order_acquire)", host)
        self.assertIn("g_enabled.store(enabled != 0, std::memory_order_release)", host)
        self.assertIn("if (!dxvk::g_enabled.load(std::memory_order_acquire))", host)

    def test_262_host_records_luma_through_dxvk_context_without_manual_submit(self):
        host = ROOT / "android/app/jni/snapframe/snapframe_host_262.cpp"
        host_text = host.read_text(encoding="utf-8")
        for required in (
            "DxvkContextObjects",
            "DxvkShaderCreateInfo",
            "new DxvkShader(shaderInfo, std::move(shaderCode))",
            "bindResourceImageView",
            "bindResourceSampler",
            "dxvkContext->dispatch(",
            "VK_FORMAT_R16_SFLOAT",
            "SNAPFRAME_DXVK262_ABI_VERSION",
            "snapframe_dxvk262_release_device",
            "queryFormatProperties",
            "queryImageFormatProperties",
            "vkGetPhysicalDeviceFormatProperties",
            "vkGetPhysicalDeviceImageFormatProperties",
        ):
            self.assertIn(required, host_text)
        for forbidden in (
            "vkQueueSubmit",
            "vkDeviceWaitIdle",
            "vkWaitForFences",
            "flushCommandList",
            "waitForIdle",
            "mapPtr(",
        ):
            self.assertNotIn(forbidden, host_text)
        self.assertNotIn("adapter()->formatProperties(", host_text)
        self.assertNotIn("adapter()->imageFormatProperties(", host_text)
        self.assertIn("source_view_ref", host_text)
        self.assertIn("Rc<DxvkImageView>", host_text)

    def test_262_host_records_bidirectional_flow_without_enabling_fake_fg(self):
        host = ROOT / "android/app/jni/snapframe/snapframe_host_262.cpp"
        host_text = host.read_text(encoding="utf-8")
        for required in (
            "VK_FORMAT_R16G16B16A16_SFLOAT",
            "k_block_match_flow_word_count",
            "state.flowImages",
            "state.flowViews",
            "state.flowShader",
            "state.hasPreviousLuma",
            "const uint32_t previousSlot = slot ^ 1u",
            "Current -> previous",
            "Previous -> current",
            "state.flowViews[0]",
            "state.flowViews[1]",
            "++state.flowPairsRecorded",
            "flow_pairs=%llu",
            "flowStride = 16u",
            "k_flow_consistency_word_count",
            "state.consistencyImage",
            "state.consistencyView",
            "state.consistencyShader",
            "consistencyBindings",
            "color_history_copy_word_count",
            "state.colorHistoryImages",
            "state.colorHistoryViews",
            "state.colorHistoryShader",
            "midpoint_warp_word_count",
            "state.midpointImage",
            "state.midpointView",
            "state.midpointWarpShader",
            "midpointBindings",
            "state.colorHistoryViews[previousSlot]",
            "state.colorHistoryViews[slot]",
            "state.consistencyView",
        ):
            self.assertIn(required, host_text)
        self.assertEqual(host_text.count(
            "dxvkContext->dispatch((state.flowWidth + 7u) / 8u"), 3)
        self.assertEqual(host_text.count("dxvkContext->dispatch("), 6)
        self.assertIn("return 0;", host_text)
        self.assertIn("does not claim a generated WSI present", host_text)
        for forbidden in (
            "vkQueueSubmit",
            "vkDeviceWaitIdle",
            "vkWaitForFences",
            "flushCommandList",
            "waitForIdle",
            "mapPtr(",
        ):
            self.assertNotIn(forbidden, host_text)

    def test_generated_present_is_blocked_until_presenter_owns_separate_wsi_transaction(self):
        design = PRESENTATION_DESIGN.read_text(encoding="utf-8")
        host = (ROOT / "android/app/jni/snapframe/snapframe_host_262.cpp").read_text(encoding="utf-8")
        # DXVK 2.6.2 currently has one acquire and one present in the D3D9
        # transaction. Do not regress into reusing its mutable image/semaphore
        # state for a synthetic second present.
        self.assertIn("m_presentPending", design)
        self.assertIn("m_frameIndex", design)
        self.assertIn("single acquire / single command-list submission / single present", design)
        self.assertIn("distinct WSI image", design)
        self.assertIn("semaphore slot", design)
        self.assertIn("current hook does not claim a generated frame was presented", design)
        self.assertIn("does not claim a generated WSI present", host)
        for forbidden in (
            "vkQueuePresentKHR",
            "presentImage(",
            "acquireNextImage(",
            "vkQueueSubmit",
            "vkDeviceWaitIdle",
            "vkWaitForFences",
        ):
            self.assertNotIn(forbidden, host)

    def test_pinned_dxvk_submission_queue_contract_is_guarded(self):
        # The pinned tree implements DxvkSubmissionQueue in dxvk_queue.*.
        # The Android patcher must validate this exact queue architecture
        # before applying any SnapFrame integration.
        self.assertIn('queue_header = (dxvk_dir / "dxvk_queue.h")', self.patcher)
        self.assertIn('queue_cpp = (dxvk_dir / "dxvk_queue.cpp")', self.patcher)
        for required in (
            "m_submitThread",
            "m_finishThread",
            "m_submitQueue",
            "m_mutexQueue",
            "entry.present.presenter->presentImage(",
            "entry.present.presenter->signalFrame(",
            "m_finishQueue.push(std::move(entry));",
            "m_finishQueue.pop();",
            "D3D9 source submit/present ordering changed",
            "DxvkSubmissionQueue FIFO submit worker + finish-thread retirement.",
        ):
            self.assertIn(required, self.patcher)

        # Current source closure queues command submission and then present;
        # the submission queue's submit thread serializes the Vulkan calls.
        self.assertIn("m_submitQueue.push(std::move(entry));", self.patcher)
        self.assertIn("std::lock_guard<dxvk::mutex> lock(m_mutexQueue);", self.patcher)
        self.assertIn("entry.submit.cmdList->submit(", self.patcher)
        self.assertIn("entry.present.presenter->presentImage(", self.patcher)
        self.assertIn("entry.present.presenter->signalFrame(", self.patcher)
        self.assertIn("Presenter::presentImage boundaries changed; re-review WSI blocking behavior", self.patcher)
        self.assertIn("DXVK WSI command-list contract changed", self.patcher)
        self.assertIn("m_commandSubmission.waitSemaphore(m_wsiSemaphores.acquire,", self.patcher)
        self.assertIn("m_commandSubmission.signalSemaphore(m_wsiSemaphores.present,", self.patcher)
        self.assertIn("m_cmd->setWsiSemaphores(sync);", self.patcher)
        self.assertIn("DXVK immutable WSI transaction contract changed", self.patcher)
        self.assertIn("sync.transactionId = m_nextTransactionId++;", self.patcher)
        self.assertIn("transaction.transactionId != m_activeTransactionId", self.patcher)
        self.assertIn("info.pImageIndices      = &presentImageIndex;", self.patcher)
        self.assertIn("presentInfo.transaction = transaction;", self.patcher)
        self.assertIn("cDevice->presentImage(cPresenter, cSync, cLatency, cFrameId, nullptr);", self.patcher)
        self.assertIn("m_activeTransactionId = 0u;", self.patcher)
        self.assertIn("m_surfaceCond.notify_one();", self.patcher)
        self.assertIn("retire it so acquire waiters cannot deadlock", self.patcher)
        self.assertIn("m_swapchainGeneration++", self.patcher)
        self.assertIn("transaction.semaphoreIndex >= m_semaphores.size()", self.patcher)
        self.assertIn("transaction.imageIndex >= m_images.size()", self.patcher)
        self.assertIn("waitForSwapchainFence(nextSync);", self.patcher)
        self.assertIn("std::numeric_limits<uint64_t>::max(),", self.patcher)
        self.assertIn("vkAcquireNextImageKHR(", self.patcher)
        self.assertIn("blocking pre-acquire behavior changed", self.patcher)
        self.assertIn("SnapFrame generated present: DISABLED pending a DXVK-owned asynchronous transaction scheduler.", self.patcher)
        model = (ROOT / "tools/tests/test_snapframe_presenter_transaction_model.py").read_text(encoding="utf-8")
        self.assertIn("class SourcePrioritySchedulerModel", model)
        self.assertIn("if self.source_pending or self.generated_running:", model)
        self.assertIn("generated_dropped", model)

    def test_main_262_runtime_pair_is_opt_in_and_links_only_matching_262_backend(self):
        game = self.game_text
        self.assertIn("option(SNAP_BUILD_DXVK_NATIVE_262_RUNTIME_PAIR", game)
        self.assertRegex(
            game,
            r"option\(SNAP_BUILD_DXVK_NATIVE_262_RUNTIME_PAIR[\s\S]*?OFF\)",
        )
        self.assertIn(
            "if(TARGET dxvk_d3d9_262 AND SNAP_BUILD_DXVK_NATIVE_262_RUNTIME_PAIR)",
            game,
        )
        self.assertIn("add_library(main_262 SHARED", game)
        self.assertIn("target_link_libraries(main_262 PRIVATE SDL2 dxvk_d3d9_262", game)
        self.assertNotIn(
            "target_link_libraries(main_262 PRIVATE SDL2 dxvk_d3d9 snapframe_host",
            game,
        )
        self.assertNotIn("target_link_libraries(main_262 PRIVATE SDL2 dxvk_d3d9 ", game)
        self.assertIn('OUTPUT_NAME "main_262"', game)
        self.assertIn("if(LB_HAS_FFMPEG)", game)
        self.assertIn("target_sources(main_262 PRIVATE", game)

    def test_apk_verifies_both_262_libraries_and_the_exact_runtime_dependency(self):
        workflow = (ROOT / ".github/workflows/android-ci.yml").read_text(encoding="utf-8")
        self.assertIn("Verify packaged DXVK 2.6.2 runtime selection and ABI", workflow)
        self.assertIn("DXVK_NATIVE_262_RUNTIME_PAIR_ABI_OK", workflow)
        self.assertIn("DXVK_NATIVE_262_RUNTIME_PAIR_REMAINS_OPT_IN", workflow)
        self.assertIn("lib/arm64-v8a/libdxvk_d3d9_262.so", workflow)
        self.assertIn("lib/arm64-v8a/libmain_262.so", workflow)
        self.assertIn("Shared library: [libdxvk_d3d9_262.so]", workflow)
        self.assertIn("main_262 does not depend on the matching DXVK 2.6.2 DSO", workflow)
        self.assertIn("BUILD_RUNTIME_PAIR", workflow)

    def test_legacy_snapframe_host_is_not_linked_to_262_candidate(self):
        # The 2.6.2 host is compiled into dxvk_d3d9_262. The game runtime
        # must link that matching DSO, never the legacy 1.9.2b host target.
        self.assertIn("target_link_libraries(main PRIVATE SDL2 dxvk_d3d9 snapframe_host", self.game_text)
        self.assertIn("target_link_libraries(snapframe_host PUBLIC dxvk_d3d9", self.snapframe_text)
        self.assertIn("snapframe_host_262.cpp", self.snapframe_text)
        self.assertNotIn("snapframe_host_262", self.game_text)
        self.assertNotIn("dxvk_d3d9_262 snapframe_host", self.game_text + self.snapframe_text)


    def test_dxvk_startup_diagnostics_are_written_under_game_port_directory(self):
        runtime_main = (ROOT / "runtime/main.c").read_text(encoding="utf-8")
        self.assertIn('setenv("DXVK_LOG_PATH", dxvk_log_path, 1);', runtime_main)
        self.assertIn('setenv("DXVK_LOG_LEVEL", "info", 1);', runtime_main)
        self.assertIn('snprintf(dxvk_log_path, sizeof(dxvk_log_path), "%s/port", g_game_dir);', runtime_main)

    def test_262_runtime_selects_the_built_sdl2_wsi_backend_before_platform_init(self):
        runtime_main = (ROOT / "runtime/main.c").read_text(encoding="utf-8")
        self.assertIn("SNAP_DXVK_NATIVE_262_RUNTIME", self.game_text)
        self.assertIn("SNAP_DXVK_NATIVE_262_RUNTIME", runtime_main)
        self.assertIn('setenv("DXVK_WSI_DRIVER", "SDL2", 1)', runtime_main)
        self.assertIn('getenv("DXVK_WSI_DRIVER")', runtime_main)
        self.assertIn('DXVK_WSI_DRIVER=SDL2', runtime_main)
        self.assertLess(
            runtime_main.index('setenv("DXVK_WSI_DRIVER", "SDL2", 1)'),
            runtime_main.index("platform_init();"),
        )

    def test_262_d3d9_startup_exceptions_do_not_cross_the_exported_abi(self):
        # DXVK 2.6.2 can throw while constructing DxvkInstance inside
        # Direct3DCreate9. Keep the exception inside the C ABI boundary and
        # preserve its message in Android logcat for diagnosis.
        self.assertIn("d3d9_main = root / \"src/d3d9/d3d9_main.cpp\"", self.patcher)
        self.assertIn('#include <exception>', self.patcher)
        self.assertIn('return D3DERR_NOTAVAILABLE;', self.patcher)
        self.assertIn("DXVK_NATIVE_262_D3D9_EXCEPTION_BARRIER_OK", self.patcher)
        self.assertIn("catch (const DxvkError& e)", self.patcher)
        self.assertIn("catch (const std::exception& e)", self.patcher)
        self.assertIn("D3DERR_NOTAVAILABLE", self.patcher)
        self.assertIn("__android_log_print(ANDROID_LOG_ERROR, \"DXVK262\"", self.patcher)
        self.assertIn("e.message().c_str()", self.patcher)
        self.assertIn("e.what()", self.patcher)

    def test_262_runtime_adapter_exposes_only_explicitly_supported_settings(self):
        runtime = ROOT / "android/app/jni/snapframe/snapframe_runtime_262.cpp"
        runtime_text = runtime.read_text(encoding="utf-8")
        for symbol in (
            "snapframe_config_defaults", "snapframe_host_set_log_path",
            "snapframe_host_set_gpu_log_path", "snapframe_host_set_flow_log_path",
            "snapframe_host_set_flow_vectors_log_path", "snapframe_host_set_event_log_path",
            "snapframe_host_log", "snapframe_host_init", "snapframe_host_shutdown",
            "snapframe_host_active", "snapframe_host_set_enabled", "snapframe_host_get_stats",
            "snapframe_host_get_settings", "snapframe_host_get_present_mode",
            "snapframe_host_update_present_mode", "snapframe_host_update_settings",
            "snapframe_host_get_last_telemetry",
        ):
            self.assertIn(symbol, runtime_text)
            self.assertIn(symbol, self.patcher)
        self.assertIn("return -ENOTSUP;", runtime_text)
        self.assertIn("kSupportedFlowScale = 50", runtime_text)
        self.assertIn("SNAPFRAME_PRESENT_MODE_AUTO", runtime_text)
        self.assertIn("snapframe_dxvk262_set_enabled", runtime_text)
        self.assertIn("generated WSI present remains disabled", runtime_text)
        self.assertIn("snapframe_runtime_262.cpp", self.snapframe_text)
        self.assertNotIn("snapframe_host.cpp", runtime_text)
        self.assertNotIn("vkQueueSubmit", runtime_text)
        self.assertNotIn("vkDeviceWaitIdle", runtime_text)


if __name__ == "__main__":
    unittest.main()
