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


## Performance/Ai implementation status — 2026-11-04

The current measured user A/B indicates SnapFrame's additional compute cost reduces source FPS from 20–24 to 10–12, while generated FPS adds 10–12 and output remains 20–24. The immediate priority is preserving source FPS, not maximizing generated count.

Fresh configurations now default to flow scale 50% and profiling disabled. This reduces flow-grid work relative to 75% and avoids diagnostic query/readback overhead in normal gameplay. Existing per-game config overrides these defaults.

The GPU scalar metrics path exists, but the Snap AI Engine controller is not yet instantiated in the live runtime. The next step is a bounded, frame-ID-matched AI policy loop that adjusts only controls actually supported by the V1 backend, beginning with flow scale and conservative hysteresis. AI must bypass on invalid/stale telemetry and must not block Present.

x3/x4 is not yet exposed. Current code generates one t=0.5 midpoint per real frame; true x3/x4 needs phase-parameterized warps, multiple output resources and safe multiple WSI presents. A multiplier-only UI/config change would duplicate the same midpoint and is explicitly rejected.


## Live Android wiring status (2026-11-04)

The `SnapAI::Engine` controller is instantiated in the Skyrim DXVK SnapFrame host.
Frame-tagged GPU scalar readback and exact-frame GPU timestamp are combined into
`SnapAIFrameMetrics`. Only the controller's flow-scale decision is currently applied.
It is queued from the asynchronous completion callback and consumed on the serialized
render callback, where history reset is requested safely. AI bypasses when telemetry
is not exact-frame valid. The initial policy is clamped to 50..75% with 8-frame warmup
and hysteresis. Other decision fields are not yet wired to shader dispatch.
