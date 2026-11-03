# Snap AI Engine integration

## Current status

The portable Snap AI Engine is fetched at a pinned upstream commit, built as a separate Android shared library, explicitly loaded before the game runtime, and its API version is checked/logged during SnapFrame bootstrap. The AI controller is not yet instantiated as an active policy loop or connected to live SnapFrame telemetry/DXVK dispatch decisions. No AI-controlled GPU savings are claimed.

Upstream source: `Snap888/Snap_AI_Engine`, commit `f60acfcb7b09722d5927215957c46045b11f7897`.

## Responsibilities

Snap AI is a scalar policy controller only. It may recommend bounded flow scale, search-radius scale, refinement passes, bidirectional-flow execution and artifact-check execution. It must never own or mutate Vulkan resources, command buffers, queues, fences, semaphores, swapchains or present scheduling.

## C API

Header: `snap_ai/snap_ai.h`.

- Create/destroy: `snap_ai_create`, `snap_ai_destroy`.
- Per-frame input/output: `snap_ai_update`.
- Hot setters: `snap_ai_set_enabled`, `snap_ai_set_profile`.
- Stats: `snap_ai_get_stats`.

Profiles exposed by API v1: Performance, Balanced and Quality. The user-facing Custom profile must remain hidden until a real host-side parameter editor and validation path exist.

## Telemetry contract

The host should pass only scalar data: GPU timing, motion magnitude/complexity, flow confidence, GPU pressure, artifact risk and scene-cut state. Invalid or missing telemetry must produce a safe bypass decision. AI output must be clamped against the backend's actual supported ranges before it is applied.

Do not perform synchronous GPU readback or pixel transfer to feed AI. Prefer asynchronously completed timestamp queries and GPU-side scalar reductions.

## Runtime behavior

- AI disabled/unavailable/error: keep the baseline SnapFrame configuration.
- Frame Generation disabled: do not dispatch the SnapFrame GPU pipeline.
- Scene cut or invalid telemetry: reset AI warmup/history and avoid aggressive recommendations.
- Apply recommendations only at a serialized, safe render-thread boundary, preferably for the next real frame.

## Remaining work

Implement a per-device runtime owner, connect scalar telemetry, clamp/apply decisions through the DXVK adapter, add bounded decision logging and test hot enable/profile changes during gameplay.
