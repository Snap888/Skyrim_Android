# AI adaptive control

## Current status

Snap AI Engine's deterministic telemetry-driven policy C API is linked, but no live control loop is connected yet. It is not a trained neural network and does not generate/interpolate pixels.

## Inputs and outputs

The current API accepts frame id, frame time, GPU time, motion magnitude, motion complexity, flow confidence, GPU pressure, artifact risk, scene-cut and telemetry-valid flags.

It can recommend normalized flow scale, search-radius scale, confidence threshold, refinement passes, previous-flow reuse, bidirectional-flow execution and artifact-check execution.

## Safe application

- Validate API version and struct sizes.
- Reject non-finite, negative or stale metrics.
- Reject decisions for an unexpected frame id.
- Clamp all recommendations to the selected backend's capabilities.
- Apply decisions only at a serialized render-thread boundary.
- Keep AI disabled after any AI error; SnapFrame must continue with its last known safe configuration.
- Reset warmup/history after scene cuts, invalid telemetry or AI reinitialization.

The `estimated_compute_saving` field is a heuristic estimate, not a measured GPU saving. Never report it as a benchmark.

## Missing work

Connect asynchronous GPU timing and safe scalar motion/confidence telemetry to the API; add hysteresis and sampling controls; log decisions; and verify that recommendations alter actual GPU dispatches without causing visible quality regressions.
