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
- `TaaJitterLayout::ndcOffsetForFrameIndex(frame, w, h, length)` — NDC jitter for a monotonic frame counter
- `TaaJitterLayout::fillHaltonSequence(length, out)` — fills a Halton (2,3) table; returns false on null/invalid length
- `TaaJitter` honours `TaaJitterDesc::sequence_length` (default 8) when advancing and wrapping
- `TaaJitter::syncToFrameIndex(frame)` — align jitter state to a wrapped monotonic frame counter
- `TaaJitter::monotonicFrameIndex()` — monotonic frame counter incremented by `advance`, set by `syncToFrameIndex`

## History validity (B5.9 deepen)

- `TaaHistoryBuffer::hasValidHistory()` — false until the first successful resolve
- `TaaHistoryBuffer::needsWarmup()` — inverse of `hasValidHistory` for resolve warm-up gating
- `TaaHistoryBuffer::accumulatedFrames()` — monotonic frame counter reset on invalidate/resize
- `TaaHistoryBuffer::invalidateGeneration()` — bumped on invalidate/resize for stale-history detection
- `TaaHistoryBuffer::isHistoryStale(observedGeneration)` — true when a consumer's epoch differs from current history
- `TaaHistoryBuffer::matchesDimensions(w, h)` — true when resolve dimensions match allocated history
- `clampTaaParams(params)` — clamps blend/rejection/gamma knobs to safe ranges
- `computeEffectiveBlend(firstFrame, params)` — 1.0 on warm-up frame, else clamped `blend_factor`
- `taaResolveRequiresVelocity/Depth(params)` — true when rejection thresholds require G-buffer surfaces
- `TaaHistoryBuffer::invalidateHistory()` — clears validity (called on resize)
- `TaaResolveStats::first_frame` — set when resolve runs before history is warm
- `TaaResolveStats::effective_blend` — 1.0 on first warm-up frame, else `TAAParams::blend_factor`
- `TaaResolveStats::skipped` / `skip_reason` — set when resolve bails before history update
- `TaaResolveSkipReason::DimensionMismatch` — resolve dimensions differ from history buffer allocation
- `TaaResolveSkipReason::MissingVelocityBuffer` / `MissingDepthBuffer` — rejection enabled but surface missing when `enforce_rejection_surfaces` is set
- `TaaResolveSkipReason::StaleHistoryGeneration` — `observed_history_generation` differs from `invalidateGeneration()`
- `taaResolveSkipReasonLabel(reason)` — stable string label for skip reasons
- `classifyTaaResolveSkip(desc, history)` — public skip classifier (same ordering as `wouldSkip`)
- `taaResolveDimensionsValid(w, h)` / `taaResolveDimensionsMatch(desc, history)` — dimension preflight helpers
- `taaResolveBypassesHistoryGenerationGuard(desc)` — true when `observed_history_generation` uses the no-guard sentinel
- `taaResolveSkipReasonIsBlocking(reason)` — true when resolve would bail before history update
- `stampObservedHistoryGeneration(desc, history)` — fill observed generation from history when sentinel is set
- `TaaResolve::wouldSkip(desc, history, &reason)` — preflight skip check without mutating history
- `TaaResolve::resetBookkeeping()` — clears resolve stats/message (called on `TaaPass::destroy` / `invalidateHistory`)
- `TaaPass::invalidateHistory()` — clears history validity and resolve bookkeeping without destroying buffers
- `TaaPass::resize(w, h)` — resizes history targets and invalidates accumulated frames
- `TaaPass::matchesDimensions(w, h)` — true when pass and history dimensions align
- `TaaPass::needsHistoryWarmup()` — true until the first successful resolve
- `TaaPass::wouldSkipResolve(desc, &reason)` — pass-level preflight skip check
- `TaaPass::historyInvalidateGeneration()` — current history invalidate epoch
- `TaaPass::stampObservedHistoryGeneration(desc)` — stamp observed generation from pass history
- `TaaPass::syncJitterToFrameIndex(frame)` — align pass jitter to a monotonic frame counter

## History warmup / reuse preflight (B5.9 deepen)

- `taaHistoryIsWarmupFrame(history)` — true when history is ready but not yet warmed
- `preflightTaaHistoryWarmup(history)` — true when history buffers are allocated
- `preflightTaaHistoryReuse(history, observedGeneration)` — true when temporal reuse is allowed
- `TaaHistoryBuffer::isWarmupFrame()` / `preflightReuse(observedGeneration)` — buffer-level warmup/reuse preflight

## Jitter sync preflight (B5.9 deepen)

- `TaaJitterLayout::expectedSlotForMonotonicFrame(frame, length)` — expected Halton slot for a monotonic counter
- `TaaJitterLayout::monotonicFrameMatchesSlot(frame, slot, length)` — true when slot matches expected
- `TaaJitter::syncToFrameIndexIfReady(frame)` — guarded sync; returns false when sequence invalid
- `TaaJitter::isSyncedToFrameIndex(frame)` — true when jitter state matches expected slot
- `TaaPass::syncJitterToFrameIndexIfReady(frame)` / `isJitterSyncedTo(frame)` — pass-level sync preflight

## Resolve blend preflight (B5.9 deepen)

- `TaaResolveBlendPreflightRejectReason` — `HistoryNotReady`, `InvalidWeights`, `ReusePolicyViolation`
- `diagnoseTaaResolveBlendPreflight(desc, history)` — classify blend preflight reject reason
- `preflightTaaResolveBlend(desc, history, &reason)` — false when history not ready or weights violate reuse policy
- `TaaPass::isWarmupResolveFrame()` / `preflightHistoryReuse(observedGeneration)` / `preflightResolveBlend(desc, &reason)` — pass-level preflight

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
