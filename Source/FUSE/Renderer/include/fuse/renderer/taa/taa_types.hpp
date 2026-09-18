#pragma once

#include <fuse/types.hpp>

namespace fuse::renderer {

/// TAA tuning parameters (B5.9 — P5 §5.9).
struct TAAParams {
    f32 blend_factor = 0.1f;
    f32 velocity_rejection = 0.9f;
    f32 depth_rejection = 0.1f;
    f32 clamp_gamma = 1.25f;
    bool use_catmull_rom = true;
};

/// Halton sub-pixel jitter sequence configuration.
struct TaaJitterDesc {
    u32 sequence_length = 8;
};

/// Forward declaration — defined in `taa_history.hpp`.
class TaaHistoryBuffer;

/// Ping-pong history buffer allocation parameters.
struct TaaHistoryBufferDesc {
    u32 width = 1;
    u32 height = 1;
};

/// CUDA surface inputs for the resolve pass (opaque until B2.6 interop wiring).
struct TaaResolveSurfaces {
    void* current_frame = nullptr;
    void* velocity_buffer = nullptr;
    void* depth_buffer = nullptr;
    void* output = nullptr;
};

/// Sentinel for `TaaResolveDesc::observed_history_generation` — skip stale-history guard.
static constexpr u32 kTaaResolveNoHistoryGeneration = 0xFFFFFFFFu;

/// Per-frame resolve request — consumed by `TaaResolve` / `TaaPass`.
struct TaaResolveDesc {
    TaaResolveSurfaces surfaces{};
    TAAParams params{};
    u32 width = 0;
    u32 height = 0;
    /// When not `kTaaResolveNoHistoryGeneration`, resolve bails if this differs from `invalidateGeneration()`.
    u32 observed_history_generation = kTaaResolveNoHistoryGeneration;
    /// When true, resolve requires velocity/depth surfaces when rejection params are active.
    bool enforce_rejection_surfaces = false;
};

/// History accumulation bookkeeping tracked alongside ping-pong targets.
struct TaaHistoryValidity {
    bool hasValidHistory = false;
    u32 accumulatedFrames = 0;
    /// Bumped on invalidate/resize — consumers can detect stale history reads.
    u32 invalidateGeneration = 0;
};

/// Why a resolve request bailed before history update (B5.9 deepen).
enum class TaaResolveSkipReason : u8 {
    None = 0,
    HistoryNotReady,
    InvalidDimensions,
    DimensionMismatch,
    MissingSurfaces,
    MissingVelocityBuffer,
    MissingDepthBuffer,
    StaleHistoryGeneration,
};

/// Clamp TAA tuning knobs to safe ranges for the CPU resolve stub.
TAAParams clampTaaParams(const TAAParams& raw);
/// True when `raw` is already within clamp ranges (no normalization required).
bool taaParamsInRange(const TAAParams& raw);
/// Clamp `params` in place — mirrors `clampTaaParams` without returning a copy.
void normalizeTaaParams(TAAParams& params);
/// True when `velocity_rejection` is active and resolve must receive a velocity surface.
bool taaResolveRequiresVelocity(const TAAParams& params);
/// True when `depth_rejection` is active and resolve must receive a depth surface.
bool taaResolveRequiresDepth(const TAAParams& params);
/// Blend weight applied this frame — 1.0 on first warm-up frame, else clamped `blend_factor`.
f32 computeEffectiveBlend(bool firstFrame, const TAAParams& params);
/// History contribution weight — complement of `effectiveBlend`, clamped to [0, 1].
f32 computeHistoryBlend(f32 effectiveBlend);

/// Current/history blend weights for one resolve frame (B5.9 deepen).
struct TaaBlendWeights {
    f32 current = 0.f;
    f32 history = 0.f;
};

/// Compute current/history blend weights for a resolve frame (B5.9 deepen).
TaaBlendWeights computeTaaBlendWeights(bool firstFrame, const TAAParams& params);
/// True when blend weights are within [0, 1] and sum to ~1 (B5.9 deepen).
bool taaBlendWeightsValid(const TaaBlendWeights& weights);
/// True when history is warm and ready for temporal reuse (B5.9 deepen).
bool taaHistoryCanReuse(const TaaHistoryBuffer& history);
/// True when history reuse is allowed for the observed invalidate epoch (B5.9 deepen).
bool taaHistoryReuseAllowed(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when resolve may sample prior history this frame (B5.9 deepen).
bool taaResolveCanReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history contribution is allowed this frame (B5.9 deepen).
bool taaHistoryBlendAllowed(bool firstFrame, const TaaHistoryBuffer& history);
/// True when history still needs warm-up before temporal reuse (B5.9 deepen).
bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history);
/// Blend weights for a resolve frame considering warm-up and reuse guards (B5.9 deepen).
TaaBlendWeights computeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve would apply a non-zero history blend weight (B5.9 deepen).
bool taaResolveAppliesHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when blend weights align with the history reuse policy (B5.9 deepen).
bool taaBlendWeightsConsistentWithReuse(const TaaBlendWeights& weights, bool historyBlendAllowed);

/// Why history temporal reuse is blocked (B5.9 deepen).
enum class TaaHistoryReuseBlockReason : u8 {
    None = 0,
    NotReady,
    NotWarm,
    StaleGeneration,
};
/// Human-readable label for history reuse block reasons (B5.9 deepen).
const char* taaHistoryReuseBlockReasonLabel(TaaHistoryReuseBlockReason reason);
/// Classify why history reuse is blocked for an observed invalidate epoch (B5.9 deepen).
TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlock(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when history temporal reuse is allowed for the observed invalidate epoch (B5.9 deepen).
bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                              TaaHistoryReuseBlockReason* reason = nullptr);
/// True when history buffers are allocated and ready for resolve (B5.9 deepen).
bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history);
/// Frames remaining before temporal reuse is allowed — 0 when warmed (B5.9 deepen).
u32 taaHistoryWarmupFramesRemaining(const TaaHistoryBuffer& history);
/// True when history is allocated and the warm-up frame has completed (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);
/// Warm-up progress in [0, 1] — 0 before first resolve, 1 once warmed (B5.9 deepen).
f32 taaHistoryWarmupProgress(const TaaHistoryBuffer& history);
/// True when history is warmed and the observed invalidate epoch is current (B5.9 deepen).
bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration);

/// Why jitter sync to a monotonic frame counter is blocked (B5.9 deepen).
enum class TaaJitterSyncBlockReason : u8 {
    None = 0,
    InvalidSequence,
    InvalidViewport,
};
/// Human-readable label for jitter sync block reasons (B5.9 deepen).
const char* taaJitterSyncBlockReasonLabel(TaaJitterSyncBlockReason reason);
/// Classify why jitter sync is blocked for viewport + sequence (B5.9 deepen).
TaaJitterSyncBlockReason classifyTaaJitterSyncBlock(u32 width, u32 height, u32 sequenceLength = 8u);
/// True when jitter can sync to `frameIndex` for viewport + sequence (B5.9 deepen).
bool preflightTaaJitterSync(u32 /*frameIndex*/, u32 width, u32 height, u32 sequenceLength = 8u,
                            TaaJitterSyncBlockReason* reason = nullptr);

/// Why resolve blend-weight preflight rejected the request (B5.9 deepen).
enum class TaaResolveBlendRejectReason : u8 {
    None = 0,
    InvalidWeights,
    InconsistentWithReuse,
};
/// Human-readable label for resolve blend reject reasons (B5.9 deepen).
const char* taaResolveBlendRejectReasonLabel(TaaResolveBlendRejectReason reason);
/// Classify why resolve blend weights would be rejected (B5.9 deepen).
TaaResolveBlendRejectReason classifyTaaResolveBlendReject(const TaaResolveDesc& desc,
                                                          const TaaHistoryBuffer& history);
/// True when computed resolve blend weights pass validation and reuse policy (B5.9 deepen).
bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveBlendRejectReason* reason = nullptr);

/// Resolve bookkeeping returned by the stub backend.
struct TaaResolveStats {
    bool resolved = false;
    /// Set when resolve bails before history update (empty history, bad dimensions, missing surfaces).
    bool skipped = false;
    TaaResolveSkipReason skip_reason = TaaResolveSkipReason::None;
    u32 width = 0;
    u32 height = 0;
    f32 last_blend = 0.f;
    /// Blend weight applied this frame — 1.0 on first warm-up frame (no history reuse).
    f32 effective_blend = 0.f;
    /// History contribution weight — complement of `effective_blend`.
    f32 history_blend = 0.f;
    bool history_swapped = false;
    bool first_frame = false;
    bool has_valid_history = false;
    u32 accumulated_frames = 0;
    /// `TaaHistoryBuffer::invalidateGeneration()` at resolve time.
    u32 history_invalidate_generation = 0;
};

/// True when resolve stats blend weights match computed policy (B5.9 deepen).
bool taaResolveStatsBlendConsistent(const TaaResolveStats& stats, const TaaResolveDesc& desc,
                                    const TaaHistoryBuffer& history);

/// True when history allocation dimensions are non-zero (B5.9 deepen).
bool taaHistoryBufferDescValid(const TaaHistoryBufferDesc& desc);
/// True when a resize would change history buffer dimensions (B5.9 deepen).
bool taaHistoryResizeNeeded(u32 currentWidth, u32 currentHeight, u32 newWidth, u32 newHeight);

/// Human-readable label for resolve skip reasons (logging / tests).
const char* taaResolveSkipReasonLabel(TaaResolveSkipReason reason);
/// True when width and height are both non-zero (resolve dimension preflight).
bool taaResolveDimensionsValid(u32 width, u32 height);
/// True when resolve dimensions differ from allocated history (dimension-mismatch early-out).
bool taaResolveHasDimensionMismatch(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when `observed_history_generation` guard is active and epoch is stale (B5.9 deepen).
bool taaResolveHistoryGenerationIsStale(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve would bail before history update (`skip_reason != None`).
bool taaResolveSkipReasonIsBlocking(TaaResolveSkipReason reason);

} // namespace fuse::renderer
