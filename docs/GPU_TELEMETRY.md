# GPU telemetry

## Existing telemetry

The current SnapFrame host has a GPU timing CSV path based on DXVK timestamp queries and completion callbacks. It records stage-level timings without waiting for GPU completion in the normal frame path. The completed `total_chain` sample and CPU frame interval are also exposed as scalar values through `snapframe_host_get_last_telemetry`; GPU samples retain the existing 30-frame sampling cadence and arrive asynchronously.

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

The existing timestamp and diagnostic logging belongs to the DXVK host. The latest completed total GPU time and frame interval are now available as non-blocking scalar telemetry. GPU-side flow magnitude/confidence/complexity reduction and scene-cut detection are not yet implemented, and scalar telemetry is not yet passed to Snap AI. The portable `SnapFrameFrameOutput` timing fields remain zero because timestamp completion is asynchronous.
