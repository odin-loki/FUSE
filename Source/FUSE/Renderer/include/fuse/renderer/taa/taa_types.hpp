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

/// Why history reuse preflight rejected the request (B5.9 deepen).
enum class TaaHistoryReuseRejectReason : u8 {
    None = 0,
    HistoryNotReady,
    HistoryNotWarmed,
    StaleGeneration,
};

/// Why jitter sync preflight rejected the request (B5.9 deepen).
enum class TaaJitterSyncRejectReason : u8 {
    None = 0,
    InvalidSequenceLength,
    InvalidViewport,
    FrameIndexMismatch,
    SlotIndexMismatch,
};

/// Why resolve history-blend preflight rejected the request (B5.9 deepen).
enum class TaaBlendPreflightRejectReason : u8 {
    None = 0,
    HistoryNotReady,
    WarmupRequired,
    StaleGeneration,
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

/// Per-reason resolve skip breakdown for preflight bookkeeping (B5.9 deepen).
struct TaaResolveSkipCounts {
    u32 total = 0;
    u32 historyNotReady = 0;
    u32 invalidDimensions = 0;
    u32 dimensionMismatch = 0;
    u32 missingSurfaces = 0;
    u32 missingVelocityBuffer = 0;
    u32 missingDepthBuffer = 0;
    u32 staleHistoryGeneration = 0;
};

/// Clamp TAA tuning knobs to safe ranges for the CPU resolve stub.
TAAParams clampTaaParams(const TAAParams& raw);
/// True when `raw` is already within clamp ranges (no normalization required).
bool taaParamsInRange(const TAAParams& raw);
/// Clamp `params` in place — mirrors `clampTaaParams` without returning a copy.
void normalizeTaaParams(TAAParams& params);
/// True when `clampTaaParams` would change any field in `raw`.
bool taaParamsRequireClamping(const TAAParams& raw);
/// True when `velocity_rejection` is active and resolve must receive a velocity surface.
bool taaResolveRequiresVelocity(const TAAParams& params);
/// True when `depth_rejection` is active and resolve must receive a depth surface.
bool taaResolveRequiresDepth(const TAAParams& params);
/// True when either velocity or depth rejection is active.
bool taaResolveRequiresRejectionSurfaces(const TAAParams& params);
/// Blend weight applied this frame — 1.0 on first warm-up frame, else clamped `blend_factor`.
f32 computeEffectiveBlend(bool firstFrame, const TAAParams& params);
/// Blend weight with explicit history-reuse guard — forces 1.0 when history cannot be sampled.
f32 computeEffectiveBlend(bool firstFrame, bool historyReusable, const TAAParams& params);
/// True when `blend_factor` is within [0, 1] before clamping (B5.9 deepen).
bool isTaaBlendFactorInRange(f32 blend_factor);
/// True when warm-up path forces full current-frame weight (no history reuse).
bool taaUsesWarmupBlend(bool first_frame);
/// True when effective blend samples history (strictly below full-current weight).
bool taaBlendUsesHistory(f32 effective_blend);
/// True when effective blend ignores history (warm-up / stale / full-current weight).
/// True when effective blend ignores history (warm-up / full-current weight).
/// Blend weight with explicit history-reuse guard — forces 1.0 when history cannot be sampled (B5.9 deepen).
/// True when warm-up path forces full current-frame weight (no history reuse) (B5.9 deepen).
/// True when effective blend samples history (strictly below full-current weight) (B5.9 deepen).
/// True when effective blend ignores history (warm-up / stale / full-current weight) (B5.9 deepen).
bool taaBlendSkipsHistoryReuse(f32 effective_blend);
/// History contribution weight — complement of `effectiveBlend`, clamped to [0, 1].
f32 computeHistoryBlend(f32 effectiveBlend);
/// True when `blend_factor` is within [0, 1] before clamping (B5.9 deepen).
bool isTaaBlendFactorInRange(f32 blend_factor);
/// True when effective blend reuses prior history (weight strictly below 1.0).
bool taaBlendWeightReusesHistory(f32 effective_blend);
/// History contribution weight — complement of `effective_blend` (B5.9 deepen).
f32 computeHistoryContributionWeight(f32 effective_blend);
/// True when warm-up path forces full current-frame weight (no history reuse).
bool taaUsesWarmupBlend(bool first_frame);

/// Current/history blend weights for one resolve frame (B5.9 deepen).
struct TaaBlendWeights {
    f32 current = 0.f;
    f32 history = 0.f;
};

/// Compute current/history blend weights for a resolve frame (B5.9 deepen).
TaaBlendWeights computeTaaBlendWeights(bool firstFrame, const TAAParams& params);
/// Compute blend weights with an explicit history-reuse guard (B5.9 deepen).
TaaBlendWeights computeTaaBlendWeightsWithReuseGuard(bool firstFrame, bool historyReusable,
                                                     const TAAParams& params);
/// True when blend weights are within [0, 1] and sum to ~1 (B5.9 deepen).
bool taaBlendWeightsValid(const TaaBlendWeights& weights);
/// Preflight blend weights for a resolve frame without mutating history (B5.9 deepen).
TaaBlendWeights preflightTaaBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Preflight blend weights for a resolve frame; returns false when weights would be invalid.
bool preflightTaaBlendWeights(bool firstFrame, const TAAParams& params, TaaBlendWeights* out = nullptr);
/// True when resolve blend preflight passes for the given history state (B5.9 deepen).
bool taaResolveBlendPreflightPasses(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Preflight resolve blend weights; returns false when blend preflight would fail.
bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaBlendWeights* out = nullptr);
/// True when history is warm and ready for temporal reuse (B5.9 deepen).
bool taaHistoryCanReuse(const TaaHistoryBuffer& history);
/// True when history still needs a warm-up resolve before temporal reuse (B5.9 deepen).
bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history);
/// True when history warm-up is complete and targets may be sampled (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);
/// True when history is ready and warmed for temporal reuse (B5.9 deepen).
/// True when history reuse preflight passes for the observed invalidate epoch (B5.9 deepen).
bool taaHistoryReusePreflightPasses(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when allocated history still needs its first resolve warm-up (B5.9 deepen).
bool taaHistoryWarmupRequired(const TaaHistoryBuffer& history);
/// True when allocated history has completed warm-up (B5.9 deepen).
/// True when history targets are ready and the first resolve has warmed them (B5.9 deepen).
/// True when reuse is blocked by warmup or a stale observed generation (B5.9 deepen).
bool taaHistoryReuseBlocked(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when resolve would proceed but history still needs warm-up (B5.9 deepen).
bool taaResolveRequiresWarmup(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history reuse is allowed for the observed invalidate epoch (B5.9 deepen).
bool taaHistoryReuseAllowed(const TaaHistoryBuffer& history, u32 observedGeneration);
/// Preflight guard for temporal history reuse — false when unwarmed or epoch is stale (B5.9 deepen).
bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when resolve may sample prior history this frame (B5.9 deepen).
bool taaResolveCanReuseHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history contribution is allowed this frame (B5.9 deepen).
bool taaHistoryBlendAllowed(bool firstFrame, const TaaHistoryBuffer& history);
/// True when history still needs warm-up before temporal reuse (B5.9 deepen).
bool taaHistoryNeedsWarmup(const TaaHistoryBuffer& history);
/// True when history is allocated and warmed for temporal reuse (B5.9 deepen).
/// True when history warm-up is complete and temporal reuse may proceed (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);
/// Early-out when history still needs warm-up before temporal reuse (B5.9 deepen).
bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// True when history has completed warm-up and may be temporally reused (B5.9 deepen).
bool taaHistoryIsWarmed(const TaaHistoryBuffer& history);
bool taaHistoryIsWarm(const TaaHistoryBuffer& history);

/// History warm-up phase for temporal reuse gating (B5.9 deepen).
enum class TaaHistoryWarmupPhase : u8 {
    NotReady = 0,
    Cold,
    Warm,
};
/// Human-readable label for history warm-up phases (B5.9 deepen).
const char* taaHistoryWarmupPhaseLabel(TaaHistoryWarmupPhase phase);
/// Classify history warm-up phase from buffer state (B5.9 deepen).
TaaHistoryWarmupPhase classifyTaaHistoryWarmupPhase(const TaaHistoryBuffer& history);
/// True when history is in the warm phase and may be temporally reused (B5.9 deepen).
bool taaHistoryWarmupPhaseAllowsReuse(TaaHistoryWarmupPhase phase);
/// Blend weights for a resolve frame considering warm-up and reuse guards (B5.9 deepen).
TaaBlendWeights computeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve would apply a non-zero history blend weight (B5.9 deepen).
bool taaResolveAppliesHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when blend weights align with the history reuse policy (B5.9 deepen).
bool taaBlendWeightsConsistentWithReuse(const TaaBlendWeights& weights, bool historyBlendAllowed);
/// True when history targets are ready and warmed for reuse (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);

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
/// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& reason);
/// Early-out when history temporal reuse should be skipped (B5.9 deepen).
bool shouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// Alias for `shouldSkipTaaHistoryReuse` — same ordering as reuse preflight (B5.9 deepen).
bool wouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason);
/// Early-out when history still needs warm-up before temporal reuse (B5.9 deepen).
bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// True when history is ready, warmed, and generation matches for reuse (B5.9 deepen).
bool taaHistoryReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration);
/// Convenience wrapper — true when `preflightTaaHistoryReuse` would pass (B5.9 deepen).
bool canPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when a reuse block reason would prevent temporal history sampling (B5.9 deepen).
bool taaHistoryReuseBlockReasonIsBlocking(TaaHistoryReuseBlockReason reason);
/// True when history is allocated and warmed for temporal reuse (B5.9 deepen).
bool taaHistoryWarmupSatisfied(const TaaHistoryBuffer& history);
/// History reuse preflight using `desc.observed_history_generation` (sentinel bypasses epoch guard).
bool preflightTaaHistoryReuseForDesc(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaHistoryReuseBlockReason* reason = nullptr);
/// Diagnose history reuse preflight; false when reuse is blocked (B5.9 deepen).
                                 TaaHistoryReuseBlockReason& outReason);
/// Classify history reuse block for a resolve desc (respects generation guard bypass) (B5.9 deepen).
TaaHistoryReuseBlockReason classifyTaaHistoryReuseBlockForResolve(const TaaResolveDesc& desc,
                                                                 const TaaHistoryBuffer& history);
/// True when resolve desc history reuse preflight passes (B5.9 deepen).
bool preflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Diagnose resolve-desc history reuse preflight; false when reuse is blocked (B5.9 deepen).
bool tryPreflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Why history warm-up is blocked before temporal reuse (B5.9 deepen).
enum class TaaHistoryWarmupBlockReason : u8 {
    None = 0,
    NotReady,
    NeedsWarmup,
};
/// Human-readable label for history warm-up block reasons (B5.9 deepen).
const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason);
/// Classify why history warm-up is blocked (B5.9 deepen).
TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history);
/// True when history warm-up preflight passes (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history,
                               TaaHistoryWarmupBlockReason* reason = nullptr);
/// Diagnose history warm-up preflight; false when warm-up is blocked (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& outReason);
/// Early-out when history temporal reuse would be blocked (B5.9 deepen).
bool wouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// Diagnose history reuse preflight with a required reject reason (B5.9 deepen).
/// True when history warm-up is complete and ready for temporal reuse (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);
/// Classify why history reuse is blocked for a resolve request (B5.9 deepen).
/// True when history temporal reuse is allowed for a resolve request (B5.9 deepen).
/// History reuse preflight for resolve with mandatory reject-reason output (B5.9 deepen).
                                           TaaHistoryReuseBlockReason& reason);
/// Early-out when history temporal reuse should be skipped for a resolve request (B5.9 deepen).
bool shouldSkipTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history buffers are allocated and ready for resolve (B5.9 deepen).
bool taaHistoryReadyForResolve(const TaaHistoryBuffer& history);
/// Frames remaining before temporal reuse is allowed — 0 when warmed (B5.9 deepen).
u32 taaHistoryWarmupFramesRemaining(const TaaHistoryBuffer& history);
/// Early-out when history still needs warm-up before temporal reuse (B5.9 deepen).
bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// Early-out when history buffers are not allocated and ready for resolve (B5.9 deepen).
bool shouldSkipTaaHistoryResolve(const TaaHistoryBuffer& history);
/// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaHistoryReadyForResolve(const TaaHistoryBuffer& history,
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
/// True when history has completed warm-up and may contribute to temporal blend (B5.9 deepen).
bool taaHistoryIsWarmed(const TaaHistoryBuffer& history);

/// Why history warm-up preflight blocked the request (B5.9 deepen).
enum class TaaHistoryWarmupBlockReason : u8 {
    NotReady,
/// True when history is warmed and no longer needs a warm-up frame (B5.9 deepen).

    NeedsWarmup,
/// Human-readable label for history warm-up block reasons (B5.9 deepen).
const char* taaHistoryWarmupBlockReasonLabel(TaaHistoryWarmupBlockReason reason);
/// Classify why history warm-up is blocked (B5.9 deepen).
TaaHistoryWarmupBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history);
/// True when history is ready and warmed for temporal contribution (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason* reason = nullptr);
/// True when history temporal reuse is allowed for a resolve request (B5.9 deepen).
bool preflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                          TaaHistoryReuseBlockReason* reason = nullptr);

/// Why history warm-up blocks temporal reuse (B5.9 deepen).
/// Classify why history warm-up blocks temporal reuse (B5.9 deepen).
/// True when history is allocated and warmed for temporal reuse (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history,
                               TaaHistoryWarmupBlockReason* reason = nullptr);
/// Convenience wrapper — true when `preflightTaaHistoryWarmup` would pass (B5.9 deepen).
bool canPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// True when resolve may apply a non-zero history blend this frame (B5.9 deepen).
bool taaHistoryTemporalBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history targets are warmed and ready for temporal reuse (B5.9 deepen).
/// True when history has completed warm-up preflight (B5.9 deepen).

/// History warm-up phase tracked alongside ping-pong targets (B5.9 deepen).
enum class TaaHistoryWarmupPhase : u8 {
    NotAllocated = 0,
    Warm,
/// Human-readable label for history warm-up phases (B5.9 deepen).
const char* taaHistoryWarmupPhaseLabel(TaaHistoryWarmupPhase phase);
/// Classify the current history warm-up phase (B5.9 deepen).
TaaHistoryWarmupPhase classifyTaaHistoryWarmupPhase(const TaaHistoryBuffer& history);
/// True when history is warmed and ready for temporal reuse (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupPhase* phase = nullptr);
/// True when history is warmed and reuse preflight passes for the observed epoch (B5.9 deepen).
bool taaHistoryTemporalReuseReady(const TaaHistoryBuffer& history, u32 observedGeneration);

/// History warm-up classification (B5.9 deepen).
enum class TaaHistoryWarmupState : u8 {
    NotReady = 0,
    AwaitingFirstResolve,
    Complete,
/// Human-readable label for history warm-up states (B5.9 deepen).
const char* taaHistoryWarmupStateLabel(TaaHistoryWarmupState state);
/// Classify history warm-up state (B5.9 deepen).
TaaHistoryWarmupState classifyTaaHistoryWarmupState(const TaaHistoryBuffer& history);
/// True when history warm-up is complete and temporal reuse may proceed (B5.9 deepen).
/// True when history warm-up is complete; optionally reports the classified state (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState* state = nullptr);

/// Resolve blend mode for the current frame (B5.9 deepen).
enum class TaaResolveBlendMode : u8 {
    Warmup = 0,
    Steady,
    StaleForcedCurrent,
/// Human-readable label for resolve blend modes (B5.9 deepen).
const char* taaResolveBlendModeLabel(TaaResolveBlendMode mode);
/// Classify resolve blend mode considering warm-up and reuse guards (B5.9 deepen).
TaaResolveBlendMode classifyTaaResolveBlendMode(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when two blend-weight tuples are within `epsilon` (B5.9 deepen).
bool taaBlendWeightsNearEqual(const TaaBlendWeights& a, const TaaBlendWeights& b, f32 epsilon = 1e-5f);

/// Why history warm-up is incomplete (B5.9 deepen).
    NeedsResolve,
/// True when history warm-up is complete and temporal accumulation may begin (B5.9 deepen).
/// True when history has completed warm-up and may accumulate temporally (B5.9 deepen).
/// True when temporal reuse may begin for the observed invalidate epoch (B5.9 deepen).
bool tryCanBeginTemporalReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
/// True when history is ready and warmed for temporal sampling (B5.9 deepen).
/// History reuse preflight using `TaaResolveDesc::observed_history_generation` (B5.9 deepen).
bool preflightTaaResolveHistoryReuse(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,


/// Why history warm-up preflight rejected the request (B5.9 deepen).
enum class TaaHistoryWarmupRejectReason : u8 {
    AlreadyWarm,
/// Human-readable label for history warm-up reject reasons (B5.9 deepen).
const char* taaHistoryWarmupRejectReasonLabel(TaaHistoryWarmupRejectReason reason);
/// Classify why history warm-up preflight would reject (B5.9 deepen).
TaaHistoryWarmupRejectReason classifyTaaHistoryWarmupReject(const TaaHistoryBuffer& history);
/// True when history warm-up preflight passes (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason* reason = nullptr);
/// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupRejectReason& reason);
/// Early-out when history warm-up should be skipped (B5.9 deepen).

/// Classify why history warm-up is incomplete (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupBlockReason& reason);
/// Early-out when history warm-up should be skipped (still needs first resolve) (B5.9 deepen).

/// True when history is allocated and still needs its first resolve frame (B5.9 deepen).
/// Early-out when history warm-up is complete or the buffer is not ready (B5.9 deepen).
/// True when history has completed warm-up and may be temporally reused (B5.9 deepen).

/// History warm-up lifecycle for temporal reuse (B5.9 deepen).
    Ready,
/// Classify history warm-up readiness (B5.9 deepen).
/// True when history warm-up is complete (B5.9 deepen).
/// History warm-up preflight with mandatory state output (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupState& state);
/// True when history is allocated and warmed for temporal accumulation (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason* reason = nullptr);
/// History warmup preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryReuseBlockReason& reason);

                               TaaHistoryWarmupRejectReason* reason = nullptr);
/// True when temporal reuse is blocked specifically because history is not warmed (B5.9 deepen).
bool taaHistoryReuseBlockedByWarmup(const TaaHistoryBuffer& history);

/// True when history warm-up is complete and temporal reuse may begin (B5.9 deepen).
/// Early-out when history warm-up is not complete (B5.9 deepen).

/// Why resolve reuse-and-blend composite preflight rejected the request (B5.9 deepen).
enum class TaaResolveReuseBlendRejectReason : u8 {
    HistoryReuseBlocked,
    BlendWeightsRejected,
/// Human-readable label for resolve reuse-and-blend reject reasons (B5.9 deepen).
const char* taaResolveReuseBlendRejectReasonLabel(TaaResolveReuseBlendRejectReason reason);
/// Classify why resolve reuse-and-blend preflight would reject (B5.9 deepen).
TaaResolveReuseBlendRejectReason classifyTaaResolveReuseBlendReject(const TaaResolveDesc& desc,
                                                                    const TaaHistoryBuffer& history,
                                                                    u32 observedGeneration);
/// True when history reuse and blend-weight preflights both pass (B5.9 deepen).
bool preflightTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      u32 observedGeneration, TaaResolveReuseBlendRejectReason* reason = nullptr);
/// Resolve reuse-and-blend preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        u32 observedGeneration, TaaResolveReuseBlendRejectReason& reason);
/// Early-out when resolve reuse-and-blend preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveReuseAndBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// True when history is allocated and no longer needs warm-up (B5.9 deepen follow-up).

/// Read-only history warm-up diagnostics — no mutation (B5.9 deepen follow-up).
struct TaaHistoryWarmupPreflight {
    bool history_ready = false;
    bool needs_warmup = true;
    u32 warmup_frames_remaining = 1u;

    [[nodiscard]] bool isWarmed() const { return history_ready && !needs_warmup; }
    [[nodiscard]] bool canReuseHistory() const { return isWarmed(); }
/// Populate warm-up diagnostics without mutating history (B5.9 deepen follow-up).
TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// Warm-up preflight with mandatory output (B5.9 deepen follow-up).
bool tryPreflightTaaHistoryWarmup(const TaaHistoryBuffer& history, TaaHistoryWarmupPreflight& out);
/// Early-out when history still needs warm-up before temporal reuse (B5.9 deepen follow-up).

/// History warm-up lifecycle for temporal reuse guards (B5.9 deepen).
/// Classify history warm-up lifecycle (B5.9 deepen).

/// Why history warm-up is blocked before temporal reuse (B5.9 deepen).
/// Early-out when history warm-up preflight would reject (B5.9 deepen).

bool isTaaHistoryWarmupComplete(const TaaHistoryBuffer& history);

/// Combined history warm-up + reuse diagnostics (B5.9 deepen).
    u32 framesRemaining = 0;
    TaaHistoryReuseBlockReason reuseBlock = TaaHistoryReuseBlockReason::None;

    bool isWarmupComplete() const { return framesRemaining == 0u; }
    bool canReuse() const { return reuseBlock == TaaHistoryReuseBlockReason::None; }
    bool passes() const { return isWarmupComplete() && canReuse(); }

/// Combined warm-up + reuse preflight for an observed invalidate epoch (B5.9 deepen).
TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when history has completed warm-up and may contribute temporally (B5.9 deepen).
/// True when history warm-up is complete and ready for temporal contribution (B5.9 deepen).
/// True when history is allocated, warmed, and ready for temporal reuse (B5.9 deepen).
bool taaHistoryWarmupReady(const TaaHistoryBuffer& history);

/// Why history warm-up preflight rejected temporal reuse (B5.9 deepen).
/// Early-out when history warm-up should block temporal reuse (B5.9 deepen).
bool shouldSkipTaaHistoryWarmup(const TaaHistoryBuffer& history);

/// Why history warm-up is blocked (B5.9 deepen).

/// Why combined resolve+blend preflight rejected the request (B5.9 deepen).
enum class TaaResolveWithBlendRejectReason : u8 {
    ResolveBlocked,
    BlendRejected,
/// Human-readable label for combined resolve+blend reject reasons (B5.9 deepen).
const char* taaResolveWithBlendRejectReasonLabel(TaaResolveWithBlendRejectReason reason);
/// Classify why combined resolve+blend preflight would reject (B5.9 deepen).
TaaResolveWithBlendRejectReason classifyTaaResolveWithBlendReject(const TaaResolveDesc& desc,
                                                                  const TaaHistoryBuffer& history);
/// True when resolve preflight and blend-weight preflight both pass (B5.9 deepen).
bool preflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                  TaaResolveWithBlendRejectReason* reason = nullptr);
/// Combined resolve+blend preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveWithBlendRejectReason& reason);
/// Early-out when combined resolve+blend preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveWithBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Compute resolve blend weights only when resolve preflight passes (B5.9 deepen).
bool tryComputeTaaResolveBlendWeightsIfResolveReady(const TaaResolveDesc& desc,
                                                    TaaBlendWeights& outWeights,
                                           TaaHistoryReuseBlockReason& reason);
TaaHistoryReuseBlockReason classifyTaaHistoryWarmupBlock(const TaaHistoryBuffer& history);
/// True when history is ready and warmed for temporal reuse (B5.9 deepen).

/// Why resolve blend-weight preflight rejected the request (B5.9 deepen).
enum class TaaResolveBlendRejectReason : u8 {
    InvalidWeights,
    InconsistentWithReuse,
/// Human-readable label for resolve blend reject reasons (B5.9 deepen).
const char* taaResolveBlendRejectReasonLabel(TaaResolveBlendRejectReason reason);
/// Classify why resolve blend weights would be rejected (B5.9 deepen).
TaaResolveBlendRejectReason classifyTaaResolveBlendReject(const TaaResolveDesc& desc,
                                                          const TaaHistoryBuffer& history);
/// True when computed resolve blend weights pass validation and reuse policy (B5.9 deepen).
bool preflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaResolveBlendRejectReason* reason = nullptr);
/// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& reason);
/// Early-out when resolve blend-weight preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve blend weights pass validation and reuse policy (B5.9 deepen).
bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Alias for `shouldSkipTaaResolveBlend` — same ordering as blend preflight (B5.9 deepen).
bool wouldSkipTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
bool tryComputeTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaBlendWeights& outWeights, TaaResolveBlendRejectReason& reason);
/// Clamp an effective blend weight to [0, 1].
f32 clampEffectiveBlend(f32 effectiveBlend);
/// History accumulation weight — complement of the current-frame effective blend.
f32 computeHistoryBlendWeight(f32 effectiveBlend);
/// History accumulation weight from frame state and params.
f32 computeHistoryBlendWeight(bool firstFrame, const TAAParams& params);
/// History contribution weight — `1.0 - effective_blend` (B5.9 deepen).
/// Blend weight with explicit history-reuse guard — forces 1.0 when history cannot be sampled.
f32 computeEffectiveBlend(bool firstFrame, bool historyReusable, const TAAParams& params);
/// True when effective blend samples history (strictly below full-current weight).
bool taaBlendUsesHistory(f32 effectiveBlend);
/// True when effective blend ignores history (warm-up / stale / full-current weight).
bool taaBlendSkipsHistoryReuse(f32 effectiveBlend);
/// Clamp an effective blend weight to [0, 1] (B5.9 deepen).
/// True when effective blend reuses prior history (weight strictly below 1.0) (B5.9 deepen).
/// History accumulation weight — complement of the current-frame effective blend (B5.9 deepen).
/// History accumulation weight from frame state and params (B5.9 deepen).
/// True when warm-up path forces full current-frame weight (no history reuse) (B5.9 deepen).

/// History warm-up preflight snapshot (B5.9 deepen).
struct TaaHistoryWarmupPreflight {
    bool buffer_ready = false;
    bool needs_warmup = true;
    bool can_reuse = false;
    u32 invalidate_generation = 0;
    u32 accumulated_frames = 0;

    bool readyForResolve() const;
    bool warmupComplete() const;
};

/// History reuse preflight including generation epoch (B5.9 deepen).
struct TaaHistoryReusePreflight {
    TaaHistoryWarmupPreflight warmup{};
    u32 observed_generation = 0;
    bool generation_matches = false;
    bool reuse_allowed = false;

    bool canReuseHistory() const;

/// Resolve blend preflight snapshot (B5.9 deepen).
struct TaaResolveBlendPreflight {
    bool first_frame = true;
    bool history_blend_allowed = false;
    bool history_reuse_allowed = false;
    bool weights_valid = false;
    TaaBlendWeights weights{};

    bool readyForBlend() const;

TaaHistoryWarmupPreflight preflightTaaHistoryWarmup(const TaaHistoryBuffer& history);
TaaHistoryReusePreflight preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
TaaHistoryReusePreflight preflightTaaHistoryReuseForDesc(const TaaHistoryBuffer& history,
                                                         const TaaResolveDesc& desc);
TaaResolveBlendPreflight preflightTaaResolveBlend(bool firstFrame, const TAAParams& params,
TaaResolveBlendPreflight preflightTaaResolveBlendForDesc(const TaaResolveDesc& desc,

enum class TaaHistoryReuseRejectReason : u8 {
    NeedsWarmup,

/// Human-readable label for history reuse reject reasons (logging / tests).
const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason);
/// Classify why history reuse would be rejected for an observed invalidate epoch.
TaaHistoryReuseRejectReason classifyTaaHistoryReuseReject(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when history reuse is allowed; optionally reports the reject reason.
bool tryTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                        TaaHistoryReuseRejectReason* reason = nullptr);

/// Why jitter sync preflight rejected the request (B5.9 deepen).
enum class TaaJitterSyncRejectReason : u8 {
    InvalidSequence,
    InvalidViewport,
    FrameIndexMismatch,

/// Human-readable label for jitter sync reject reasons (logging / tests).
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason);

/// Resolve + blend preflight snapshot — non-mutating (B5.9 deepen).
    bool resolve_would_pass = false;
    bool blend_weights_valid = false;
    TaaResolveSkipReason resolve_skip_reason = TaaResolveSkipReason::None;
    /// True when resolve preflight passes and blend weights are valid.
    bool passes() const { return resolve_would_pass && blend_weights_valid; }

/// Preflight resolve eligibility and blend weights without mutating history.
bool preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                              TaaResolveBlendPreflight* out = nullptr);
/// True when `classifyTaaHistoryReuseBlock` would not return `None` (B5.9 deepen).
bool taaHistoryReuseBlocked(const TaaHistoryBuffer& history, u32 observedGeneration);

/// Resolve blend plan computed without mutating history (B5.9 deepen).
    bool valid = false;
    bool first_frame = false;
    bool history_reuse = false;
/// Compute expected blend weights for a resolve request (B5.9 deepen).
TaaResolveBlendPreflight preflightTaaResolveBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when a blend preflight produced valid weights (B5.9 deepen).
bool taaResolveBlendPreflightValid(const TaaResolveBlendPreflight& preflight);
/// True when resolve would contribute history weight this frame (B5.9 deepen).
bool taaResolveWouldBlendHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when blend preflight matches history warm-up state (B5.9 deepen).
bool taaResolveBlendConsistentWithHistory(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history is not ready or still needs warm-up (B5.9 deepen).
/// True when history targets are ready and warmed for temporal reuse (B5.9 deepen).
/// True when the next resolve would run before history is warm (B5.9 deepen).
bool taaResolveWouldBeFirstFrame(const TaaHistoryBuffer& history);
/// True when history is ready and warmed for temporal reuse (B5.9 deepen).
/// Classify why history reuse preflight would reject (B5.9 deepen).
/// Preflight history reuse — returns true when temporal reuse is allowed (B5.9 deepen).
bool taaHistoryReusePreflight(const TaaHistoryBuffer& history, u32 observedGeneration,
/// True when the next resolve would be the warm-up frame (B5.9 deepen).
bool computeTaaResolveFirstFrame(const TaaHistoryBuffer& history);
/// True when resolve would sample prior history this frame (B5.9 deepen).
bool taaResolveWouldUseHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Classify why resolve history-blend preflight would reject (B5.9 deepen).
TaaBlendPreflightRejectReason classifyTaaBlendPreflightReject(const TaaResolveDesc& desc,
/// Preflight resolve history-blend — returns true when history contribution is allowed (B5.9 deepen).
bool taaResolveBlendPreflight(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaBlendPreflightRejectReason* reason = nullptr);
/// True when the next resolve would be the warm-up frame (history ready, not yet valid).
bool taaHistoryIsWarmupFrame(const TaaHistoryBuffer& history);
/// True when history buffers are allocated and can accept a warm-up resolve (B5.9 deepen).
bool preflightTaaHistoryWarmup(const TaaHistoryBuffer& history);
/// True when temporal history reuse is allowed for the observed invalidate epoch (B5.9 deepen).
bool preflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);

/// Why a resolve blend-weight preflight rejected the request (B5.9 deepen).
enum class TaaResolveBlendPreflightRejectReason : u8 {
    None = 0,
    HistoryNotReady,
    ReusePolicyViolation,
/// Human-readable label for resolve blend preflight reject reasons (B5.9 deepen).
const char* taaResolveBlendPreflightRejectReasonLabel(TaaResolveBlendPreflightRejectReason reason);
/// Diagnose why resolve blend preflight would reject; vacuously succeeds on valid paths (B5.9 deepen).
TaaResolveBlendPreflightRejectReason diagnoseTaaResolveBlendPreflight(const TaaResolveDesc& desc,
/// Preflight guard before applying resolve blend weights; false when history is not ready or weights violate reuse policy (B5.9 deepen).
                              TaaResolveBlendPreflightRejectReason* reason = nullptr);
/// Preflight history reuse for an observed invalidate epoch (B5.9 deepen).
/// Preflight resolve blend weights for consistency with reuse policy (B5.9 deepen).
                              TaaBlendWeights* weights = nullptr);
/// True when resolve skip and blend-weight preflights both pass (B5.9 deepen).
bool preflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                               TaaResolveSkipReason* skipReason = nullptr,
                               TaaResolveBlendRejectReason* blendRejectReason = nullptr);
/// Convenience wrapper — true when `preflightTaaResolveBlendWeights` would pass (B5.9 deepen).
bool canPreflightTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Fill `out` only when blend-weight preflight passes; returns false when rejected (B5.9 deepen).
                                      TaaBlendWeights& out,
                                      TaaResolveBlendRejectReason* reason = nullptr);

/// Why resolve temporal-blend preflight rejected the request (B5.9 deepen).
enum class TaaResolveTemporalRejectReason : u8 {
    HistoryReuseBlocked,
    BlendWeightsRejected,
/// Human-readable label for resolve temporal-blend reject reasons (B5.9 deepen).
const char* taaResolveTemporalRejectReasonLabel(TaaResolveTemporalRejectReason reason);
/// Classify why resolve temporal-blend preflight would reject (B5.9 deepen).
TaaResolveTemporalRejectReason classifyTaaResolveTemporalReject(const TaaResolveDesc& desc,
                                                                const TaaHistoryBuffer& history);
/// True when history reuse and blend-weight preflights both pass (B5.9 deepen).
bool preflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                      TaaResolveTemporalRejectReason* reason = nullptr);
/// Convenience wrapper — true when `preflightTaaResolveTemporalBlend` would pass (B5.9 deepen).
bool canPreflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when a blend reject reason would block resolve blend-weight application (B5.9 deepen).
bool taaResolveBlendRejectReasonIsBlocking(TaaResolveBlendRejectReason reason);
/// Combined resolve skip + blend-weight preflight (B5.9 deepen).
bool preflightTaaResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// True when `weights` match `computeTaaResolveBlendWeights` for the resolve frame (B5.9 deepen).
bool taaResolveBlendWeightsMatchExpected(const TaaBlendWeights& weights, const TaaResolveDesc& desc,
/// True when resolve blend and history reuse preflights both pass (B5.9 deepen).
bool taaResolveTemporalBlendAllowed(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Combined resolve temporal-blend preflight with optional reject diagnostics (B5.9 deepen).
                                      TaaResolveBlendRejectReason* blendReason = nullptr,
                                      TaaHistoryReuseBlockReason* reuseReason = nullptr);
/// Combined resolve-frame preflight — skip check then blend-weight validation (B5.9 deepen).
                              TaaResolveBlendRejectReason* blendReason = nullptr);

/// Combined resolve-desc preflight outcome (B5.9 deepen).
struct TaaResolveDescPreflight {
    bool passes = false;
    TaaResolveSkipReason skip_reason = TaaResolveSkipReason::None;
    TaaResolveBlendRejectReason blend_reject_reason = TaaResolveBlendRejectReason::None;
/// Preflight resolve skip and blend-weight guards for one resolve request (B5.9 deepen).
bool preflightTaaResolveDesc(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                             TaaResolveDescPreflight* result = nullptr);
/// Diagnose resolve blend-weight preflight; false when blend weights are rejected (B5.9 deepen).
                                        TaaResolveBlendRejectReason& outReason);
/// Combined resolve temporal preflight — skip, reuse, and blend diagnostics (B5.9 deepen).
struct TaaResolveTemporalPreflight {
    TaaHistoryReuseBlockReason reuse_block = TaaHistoryReuseBlockReason::None;
    TaaResolveBlendRejectReason blend_reject = TaaResolveBlendRejectReason::None;
/// Preflight resolve skip, history reuse, and blend weights together (B5.9 deepen).
bool preflightTaaResolveTemporalAccumulation(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                             TaaResolveTemporalPreflight* result = nullptr);
/// Compute resolve blend weights only when preflight passes (B5.9 deepen).
                                      TaaBlendWeights& outWeights,

struct TaaResolveFramePreflight {
    bool canProceed = false;
    TaaResolveBlendRejectReason blend_reason = TaaResolveBlendRejectReason::None;
/// True when resolve would proceed past skip and blend-weight guards (B5.9 deepen).
                              TaaResolveFramePreflight* out = nullptr);
/// True when history temporal reuse would be skipped for the observed invalidate epoch (B5.9 deepen).
bool shouldSkipTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration);
/// Diagnose history reuse preflight; false when reuse would be blocked (B5.9 deepen).
bool tryPreflightTaaHistoryReuse(const TaaHistoryBuffer& history, u32 observedGeneration,
                                 TaaHistoryReuseBlockReason& outReason);
/// True when resolve may sample warmed history this frame (B5.9 deepen).
bool canSampleHistoryForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Diagnose resolve-context history reuse; false when sampling would be blocked (B5.9 deepen).
bool tryPreflightTaaHistoryReuseForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// True when `observedGeneration` differs from the current invalidate epoch (B5.9 deepen).
bool wouldInvalidateHistoryIfStale(const TaaHistoryBuffer& history, u32 observedGeneration);
/// True when history warm-up is complete (B5.9 deepen).
bool taaHistoryWarmupComplete(const TaaHistoryBuffer& history);
/// True when resolve would skip history blend this frame (B5.9 deepen).
bool shouldSkipHistoryBlendAtResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Diagnose resolve blend-weight preflight; false when weights would be rejected (B5.9 deepen).
/// True when resolve may apply a non-zero history blend weight (B5.9 deepen).
bool canApplyHistoryBlendAtResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Effective invalidate epoch for resolve reuse checks — stamps from history when sentinel is set (B5.9 deepen).
u32 effectiveObservedHistoryGeneration(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history temporal reuse is allowed for a resolve request (B5.9 deepen).
bool taaHistoryReuseAllowedForResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Resolve-desc-aware history reuse preflight (B5.9 deepen).
bool preflightTaaResolveHistoryReuse(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                     TaaHistoryReuseBlockReason* reason = nullptr);
/// True when blend preflight passes and reuse preflight passes when history blend would apply (B5.9 deepen).
bool preflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaHistoryReuseBlockReason* reuseReason = nullptr,
/// Compute resolve blend weights only when preflight passes; returns false when rejected (B5.9 deepen).
                                      TaaBlendWeights* out);
bool wouldRejectTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Diagnose resolve blend preflight with a required reject reason (B5.9 deepen).
/// True when resolve blend weights pass validation and reuse policy (B5.9 deepen).
/// True when computed resolve blend weights pass validation and reuse policy (B5.9 deepen).
bool taaResolveBlendReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Early-out when resolve skip or blend-weight preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
bool taaResolveBlendWeightsReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Early-out when resolve would apply a non-zero history blend weight (B5.9 deepen).
bool shouldSkipTaaResolveHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// Resolve history-blend preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveHistoryBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        TaaResolveBlendRejectReason& reason);

/// Why combined resolve temporal preflight rejected the request (B5.9 deepen).
/// Human-readable label for resolve temporal reject reasons (B5.9 deepen).
/// Classify why combined resolve temporal preflight would reject (B5.9 deepen).
/// Combined resolve temporal preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaResolveTemporalRejectReason& reason);
/// Early-out when combined resolve temporal preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveTemporal(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve blend weights are ready for the current history state (B5.9 deepen).
bool computeTaaResolveBlendWeightsIfReady(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// True when history reuse and resolve blend-weight preflights both pass (B5.9 deepen).
bool preflightTaaTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Early-out when temporal blend preflight would reject reuse or blend weights (B5.9 deepen).
bool shouldSkipTaaTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

/// Why combined temporal resolve preflight rejected the request (B5.9 deepen).
enum class TaaTemporalGuardRejectReason : u8 {
/// Human-readable label for temporal guard reject reasons (B5.9 deepen).
const char* taaTemporalGuardRejectReasonLabel(TaaTemporalGuardRejectReason reason);
/// Classify which temporal guard would block resolve (history reuse first, then blend) (B5.9 deepen).
TaaTemporalGuardRejectReason classifyTaaTemporalGuardReject(const TaaResolveDesc& desc,
bool preflightTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaTemporalGuardRejectReason* reason = nullptr);
/// Temporal resolve preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                    TaaTemporalGuardRejectReason& reason);
/// Early-out when temporal resolve preflight would reject (B5.9 deepen).
bool shouldSkipTaaTemporalResolve(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

/// Read-only resolve blend diagnostics — no mutation (B5.9 deepen follow-up).
    TaaResolveBlendRejectReason reject_reason = TaaResolveBlendRejectReason::None;
    bool can_apply = false;

    [[nodiscard]] bool appliesHistoryBlend() const { return weights.history > 1e-5f; }
/// Populate resolve blend diagnostics without mutating history (B5.9 deepen follow-up).

/// Why combined resolve temporal-blend preflight rejected the request (B5.9 deepen).
/// Classify why combined resolve temporal-blend preflight would reject (B5.9 deepen).
TaaResolveTemporalRejectReason classifyTaaResolveTemporalBlendReject(const TaaResolveDesc& desc,
/// Combined resolve temporal-blend preflight with mandatory reject-reason output (B5.9 deepen).
bool tryPreflightTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Early-out when combined resolve temporal-blend preflight would reject (B5.9 deepen).
bool shouldSkipTaaResolveTemporalBlend(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when history reuse and resolve blend preflights both pass (B5.9 deepen).
bool preflightTaaTemporalResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                       u32 observedGeneration,
/// Early-out when temporal resolve guards would reject blend preflight (B5.9 deepen).
bool shouldSkipTaaTemporalResolveGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                        u32 observedGeneration);
/// True when history blend is degraded to full-current this frame (B5.9 deepen).
bool isHistoryBlendDegraded(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when computed resolve blend weights pass validation and may be applied (B5.9 deepen).
bool canApplyTaaResolveBlendWeights(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);

/// Combined resolve blend-weight diagnostics for one frame (B5.9 deepen).
    TaaResolveBlendRejectReason rejectReason = TaaResolveBlendRejectReason::None;
    bool historyBlendAllowed = false;
    bool historyDegraded = false;

    bool passes() const { return rejectReason == TaaResolveBlendRejectReason::None; }

/// Combined resolve blend-weight preflight for one frame (B5.9 deepen).
TaaResolveBlendPreflight preflightTaaResolveBlendFrame(const TaaResolveDesc& desc,

/// Combined history warm-up + resolve blend guard diagnostics (B5.9 deepen).
struct TaaFrameGuardPreflight {
    TaaHistoryWarmupPreflight history{};
    TaaResolveBlendPreflight blend{};

    bool passes() const { return history.passes() && blend.passes(); }

/// Combined history warm-up + resolve blend guard preflight (B5.9 deepen).
TaaFrameGuardPreflight preflightTaaFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Combined history-reuse + blend-weight preflight for temporal resolve (B5.9 deepen).
/// Combined temporal preflight with mandatory reject-reason outputs (B5.9 deepen).
                                    TaaHistoryReuseBlockReason& reuseReason,
                                    TaaResolveBlendRejectReason& blendReason);
/// Early-out when combined temporal resolve preflight would reject (B5.9 deepen).

bool preflightTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
/// Resolve frame preflight with mandatory reject-reason outputs (B5.9 deepen).
bool tryPreflightTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                       TaaResolveSkipReason& skipReason,
/// Early-out when resolve frame guards would reject (B5.9 deepen).
bool shouldSkipTaaResolveFrameGuards(const TaaResolveDesc& desc, const TaaHistoryBuffer& history);
/// True when resolve preflight and blend-weight preflight both pass (B5.9 deepen).
                               TaaResolveBlendRejectReason* blendReject = nullptr);
/// Resolve-frame preflight with mandatory skip/blend reject outputs (B5.9 deepen).
bool tryPreflightTaaResolveFrame(const TaaResolveDesc& desc, const TaaHistoryBuffer& history,
                                 TaaResolveSkipReason& skipReason, TaaResolveBlendRejectReason& blendReject);
/// Early-out when resolve preflight or blend-weight preflight would reject (B5.9 deepen).

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
    /// Set when resolve samples ping-pong history (reusable + blend below full-current weight).
    bool history_reused = false;
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
/// True when resolve stats record valid, consistent blend weights (B5.9 deepen).
bool taaResolveStatsBlendConsistent(const TaaResolveStats& stats);

/// True when history allocation dimensions are non-zero (B5.9 deepen).
bool taaHistoryBufferDescValid(const TaaHistoryBufferDesc& desc);
/// True when a resize would change history buffer dimensions (B5.9 deepen).
bool taaHistoryResizeNeeded(u32 currentWidth, u32 currentHeight, u32 newWidth, u32 newHeight);

/// Human-readable label for history-reuse reject reasons (logging / tests).
const char* taaHistoryReuseRejectReasonLabel(TaaHistoryReuseRejectReason reason);
/// Human-readable label for jitter-sync reject reasons (logging / tests).
const char* taaJitterSyncRejectReasonLabel(TaaJitterSyncRejectReason reason);
/// Human-readable label for resolve blend preflight reject reasons (logging / tests).
const char* taaBlendPreflightRejectReasonLabel(TaaBlendPreflightRejectReason reason);
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
/// Increment `counts` for one classified skip reason (no-op when `None`).
void accumulateTaaResolveSkipReason(TaaResolveSkipCounts& counts, TaaResolveSkipReason reason);
/// Build a single-reason skip breakdown from one classified reason.
/// Increment `counts` for one classified skip reason (no-op when `None`) (B5.9 deepen).
/// Build a single-reason skip breakdown from one classified reason (B5.9 deepen).
TaaResolveSkipCounts taaResolveSkipCountsFromReason(TaaResolveSkipReason reason);

} // namespace fuse::renderer
