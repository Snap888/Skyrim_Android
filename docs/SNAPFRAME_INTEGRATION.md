# SnapFrame Engine integration

## Current status

The portable SnapFrame v1 library is pinned and linked into the Android runtime. A concrete `SnapFrameBackendV1` adapter routes DXVK frame callbacks through the portable core and invokes the real DXVK GPU pipeline using an opaque native-frame token. SnapFrame is the sole frame-generation backend. The adapter is currently a single host-level instance; migrating process-global GPU state to per-device adapter objects remains outstanding.

## Existing GPU path

The Skyrim fork already contains the original compute implementation and DXVK-specific callbacks. It performs real-frame capture/history, luma preprocessing, bidirectional block-matching flow, consistency checks and midpoint warp reconstruction. It also contains GPU timing and diagnostic logging.

Keep this implementation intact until a new adapter has output parity and lifecycle coverage. Do not replace it with the portable mock backend.

## Portable API boundary

Header: `snap_frame/snap_frame.h`.

The portable core owns configuration validation, frame sequencing, bypass/fallback state and host-visible counters. The adapter passes an opaque stack-scoped native-frame token synchronously; it is never retained. The backend delegates GPU resource ownership, shader dispatch, barriers, synchronization and generated-frame scheduling to the existing DXVK host.

API v1 supports multiplier 1 (bypass) and multiplier 2 (one midpoint). SnapFrame is the only frame-generation backend exposed by the Skyrim runtime.

## Backend migration requirements

1. **Outstanding:** move resource ownership from process-global variables to per-DXVK-device state.
2. **Implemented:** map the real DXVK frame callback to `SnapFrameFrameInput` with a monotonic frame id and opaque native token.
3. **Preserved:** existing shader algorithms and resource formats are unchanged.
4. **Implemented:** `initialize`, `update_config`, `reset_history`, `process_frame` and `shutdown` are wired through `SnapFrameBackendV1`.
5. **Preserved:** DXVK synchronization and present ownership remain in the legacy DXVK host.
6. **Preserved:** generated-frame counters update only after the DXVK present-result hook accepts the generated present. The portable output reports zero at dispatch time because acceptance is asynchronous.
7. **Preserved:** the device-destroy hook releases old resources; the host adapter rebuilds resources on the next device callback. Per-device engine instances remain outstanding.
8. **Preserved:** backend errors bypass generated output and leave the real-frame present path intact.

## Hot settings

The portable core exposes hot enable/config/quality setters. The adapter currently accepts only enable/disable and flow scale (25–100%); it rejects changes to quality, multiplier, motion analysis, profiling and diagnostic fields because those are not wired to the legacy GPU host. JNI updates the portable core config, the adapter forwards validated values to atomics and requests a history reset, and the serialized DXVK copy callback releases/recreates temporal resources with the new scale. The UI does not directly mutate Vulkan objects.

## Validation

The existing host's asynchronous GPU timestamp queries and CSV logs remain authoritative. `SnapFrameFrameOutput` timing fields are currently zero because legacy timing values arrive asynchronously; generated-frame output is also zero at dispatch time because WSI acceptance is reported later by DXVK. This avoids inventing timing or accepted-frame values.

Portable API tests do not validate Vulkan dispatch. The concrete adapter compiled for Android ARM64 in CI run #41738044737. It still requires device execution, GPU timestamp checks, generated-frame acceptance counters and visual comparison against the unchanged pre-adapter path.


## Shared-swapchain present mode

The launcher and in-game overlay write `present_mode=0..3` to `port/snapframe.conf`. The in-game overlay can request a live change:

- `0` — Auto; preserve DXVK's current VSync/tear-free selection.
- `1` — FIFO.
- `2` — MAILBOX, then FIFO fallback if unsupported.
- `3` — IMMEDIATE, then FIFO fallback if unsupported.

The choice is made by DXVK when creating the shared swapchain. Game and generated frames therefore use the same Vulkan present mode; independent source/generated present modes are not supported by this integration. The DXVK D3D9 library obtains the setting through a function-pointer callback registered by SnapFrame Host at initialization and cleared during shutdown. The overlay sends the new value through JNI to a host atomic. At the next serialized DXVK present boundary, DXVK drains outstanding source/generated submissions, waits for device idle, recreates the shared swapchain and refreshes its image views before continuing. No Vulkan object is mutated from the Android UI thread. This can cause a one-time frame-time spike. The UI displays the requested mode; actual mode/fallback must be confirmed from DXVK WSI logs.

For A/B testing, use Auto first, then FIFO/MAILBOX/IMMEDIATE with the same scene and VSync setting. Record the selected/actual present mode and compare source FPS, source frame times, generated-frame acceptance and pacing. Do not treat queued/present-submission FPS as measured panel scanout.

## Compact local log analysis

The repository includes `tools/analyze_snapframe_logs.py`, a standard-library-only Python analyzer for the SnapFrame log bundle. Run it on the device in Termux or on a PC after copying the log directory:

```sh
python3 tools/analyze_snapframe_logs.py --log-dir /path/to/port --output snapframe_diagnostic_report.md
```

For machine-readable output:

```sh
python3 tools/analyze_snapframe_logs.py --log-dir /path/to/port --format json --output snapframe_diagnostic_report.json
```

The report aggregates event counts, generated-present VkResult failures, performance mean/median/p95/p99/max, flow telemetry availability, GPU-stage sample availability, observed present modes, and prioritized findings. It reads the event/performance/flow/GPU CSV files, SnapFrame log, DXVK log and config when present. Source logs are read-only and remain on the device; the analyzer performs no network requests and does not upload anything. Share the Markdown report first; attach full logs only when a finding requires deeper tracing. The report is a triage aid, not a replacement for source-level investigation. Tests run in Android CI.

### WSI image depth for source + generated presentation

DXVK now requests at least three swapchain images for the SnapFrame shared presenter (subject to the surface's supported limits). This is intended to leave room for a source image, a queued/scan-out image, and one generated image in the same cycle. The actual selected present mode and actual image count are logged as `SnapFrame: actual present mode=... image_count=...`. This is a measured experiment, not a guaranteed FIFO fix: triple buffering can increase queue latency on some drivers. Compare Game FPS, generated acceptance rate, source acquire p95, and visible input latency across all four modes. If the surface clamps the count to two, or FIFO still reports VK_NOT_READY, the next step is the per-acquire ticket design—not longer blocking acquire timeouts on the game thread.

### WSI image-count diagnostics

The local log analyzer extracts `SnapFrame: actual present mode=... image_count=... extent=...` records and flags surfaces that clamp the requested three-image swapchain below three images. This is a diagnostic finding, not proof that image count alone causes generated-frame drops. Correlate it with `VK_NOT_READY` generated acquire events and Game FPS for each actual present mode.
