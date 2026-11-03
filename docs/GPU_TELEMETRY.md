# GPU telemetry

## Existing telemetry

The current SnapFrame host has a GPU timing CSV path based on DXVK timestamp queries and completion callbacks. It records stage-level timings without waiting for GPU completion in the normal frame path.

The existing diagnostic flow/vector path can perform host-visible readback to inspect flow fields. That path is diagnostic-only and must not become the AI telemetry path.

## AI-safe telemetry

Preferred inputs are scalar values:
- per-stage and total GPU milliseconds from completed timestamp queries;
- motion magnitude and confidence from a GPU-side reduction or another non-blocking scalar source;
- scene-cut and frame-order flags;
- generated, skipped and failed frame counters.

Do not copy pixel or flow images to CPU synchronously for AI. Do not insert queue-idle waits or fence waits into the real-frame path.

## Sampling and logging

Use configurable sampling, bounded CSV file sizes and low-frequency summaries. Avoid per-frame log spam. AI decision logs should include input validity, selected profile, output values, clamp/rejection reasons and the frame id.

## Current status

The existing timestamp and diagnostic logging belongs to the legacy host. The portable `SnapFrameFrameOutput` timing fields are not yet populated by a new DXVK adapter, and scalar telemetry is not yet passed to Snap AI.
