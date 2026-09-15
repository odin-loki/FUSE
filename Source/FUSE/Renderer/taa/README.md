# B5.9 — Temporal Anti-Aliasing (stub)

CPU-first TAA scaffolding for Track B5.9. Implements Halton sub-pixel jitter, ping-pong history buffers, and a resolve pass facade. The CUDA `taa_resolve_kernel` (variance clamp, Catmull-Rom history, velocity rejection) is deferred until B2.6 interop and G-buffer velocity wiring land.

## Layout

| Header | Role |
|--------|------|
| `taa_types.hpp` | `TAAParams`, `TaaHistoryValidity`, history/jitter/resolve descriptor types |
| `taa_jitter.hpp` | `TaaJitterLayout` Halton helpers + `TaaJitter` frame state |
| `taa_history.hpp` | Ping-pong `TextureHandle` history targets + validity flags |
| `taa_resolve.hpp` | Resolve stub — records inputs, validity, and swaps history |
| `taa_pass.hpp` | `TaaPass` facade + render-graph hook |

## Jitter sequence (B5.9 deepen)

- `TaaJitterLayout::halton(index, base)` — CPU Halton reference used by tests and custom sequence lengths
- `TaaJitterLayout::validateSequenceLength(length)` — rejects zero or >64 frame sequences
- `TaaJitterLayout::sequencePeriod(length)` — returns the jitter cycle length (0 when invalid)
- `TaaJitterLayout::frameIndexInSequence(frame, length)` — maps a monotonic frame counter into the active slot
- `TaaJitterLayout::offsetForFrameIndex(frame, length)` — Halton offset for a monotonic frame counter (wraps with period)
- `TaaJitterLayout::fillHaltonSequence(length, out)` — fills a Halton (2,3) table for projection jitter
- `TaaJitter` honours `TaaJitterDesc::sequence_length` (default 8) when advancing and wrapping

## History validity (B5.9 deepen)

- `TaaHistoryBuffer::hasValidHistory()` — false until the first successful resolve
- `TaaHistoryBuffer::accumulatedFrames()` — monotonic frame counter reset on invalidate/resize
- `TaaHistoryBuffer::invalidateHistory()` — clears validity (called on resize)
- `TaaResolveStats::first_frame` — set when resolve runs before history is warm
- `TaaResolveStats::effective_blend` — 1.0 on first warm-up frame, else `TAAParams::blend_factor`
- `TaaResolve::resetBookkeeping()` — clears resolve stats/message (called on `TaaPass::destroy`)
- `TaaPass::invalidateHistory()` — clears history validity without destroying buffers

## Pipeline (stub)

`jitter → gbuffer (velocity) → resolve → history swap`

- **Jitter** — `TaaJitter::currentNdcOffset()` feeds the projection matrix each frame
- **History** — two RGBA16F targets allocated via `ResourceManager`
- **Resolve** — validates surfaces, updates stats/validity, swaps history; kernel deferred
- **Graph** — `addTaaPassToGraph()` inserts the `taa_resolve` pass (also scheduled by `DeferredFramePipeline`)

## Tests

`fuse_taa_pass` (`ctest` name `fuse_taa_pass`) covers Halton layout helpers, sequence period/wrap, custom sequence length, history validity flags, empty-history rejection, validity reset after invalidate, ping-pong, resolve validation, `TaaPass` lifecycle, and render-graph registration.

```bash
ctest --test-dir build --output-on-failure -R fuse_taa_pass
```

## Build

Part of `fuse_rhi`. Built with `FUSE_BUILD_CORE=ON`. Tests run when `FUSE_BUILD_CORE_TESTS=ON`.
