#pragma once

#include <fuse/renderer/render_graph.hpp>
#include <fuse/renderer/taa/taa_history.hpp>
#include <fuse/renderer/taa/taa_jitter.hpp>
#include <fuse/renderer/taa/taa_resolve.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

#include <memory>
#include <string>

namespace fuse::renderer {

struct TaaPassDesc {
    u32 width = 1920;
    u32 height = 1080;
    TAAParams params{};
    TaaJitterDesc jitter{};
};

struct TaaPassStats {
    bool ready = false;
    u32 framesResolved = 0;
    fuse::math::Vec2 lastJitterNdc{};
    std::string message;
};

/// B5.9 temporal anti-aliasing pass scaffold — jitter, history, and resolve wiring.
class TaaPass {
public:
    static std::unique_ptr<TaaPass> create(const TaaPassDesc& desc = {});

    bool init(ResourceManager& resources);
    void destroy();

    bool isReady() const { return m_stats.ready; }
    const TaaPassDesc& desc() const { return m_desc; }
    const TaaPassStats& lastStats() const { return m_stats; }

    const TaaJitter& jitter() const { return m_jitter; }
    const TaaHistoryBuffer& history() const { return m_history; }
    const TaaResolve& resolve() const { return m_resolve; }

    fuse::math::Vec2 currentJitterNdc() const;
    /// NDC jitter only when viewport is valid; returns false when blocked (B5.9 deepen).
    bool currentJitterNdcIfReady(fuse::math::Vec2& out) const;
    void advanceJitter();
    /// Align jitter to a monotonic frame counter (wraps with sequence period).
    void syncJitterToFrameIndex(u32 frameIndex);
    /// Sync jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool syncJitterToFrameIndexIfReady(u32 frameIndex);
    /// True when pass jitter can align to `frameIndex` (B5.9 deepen).
    bool canSyncJitterToFrameIndex(u32 frameIndex) const;
    /// Classify why pass jitter sync would be rejected (B5.9 deepen).
    TaaJitterSyncRejectReason classifyJitterSyncReject(u32 frameIndex) const;
    /// Preflight pass jitter sync without mutating state (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    /// Sync jitter only when sequence and viewport are valid; returns false when blocked (B5.9 deepen).
    bool syncJitterToFrameIndexIfViewportReady(u32 frameIndex);
    /// Diagnose pass jitter sync; false when sync is blocked (B5.9 deepen).
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason);
    /// True when pass jitter monotonic counter and slot match `frameIndex` (B5.9 deepen).
    bool jitterAlignedToFrameIndex(u32 frameIndex) const;
    /// True when pass jitter matches the expected monotonic frame counter (B5.9 deepen).
    bool isJitterSyncedToFrameIndex(u32 frameIndex) const;
    /// Pass jitter monotonic frame counter (B5.9 deepen).
    u32 jitterMonotonicFrameIndex() const { return m_jitter.monotonicFrameIndex(); }
    /// True when pass jitter monotonic counter matches `frameIndex` (B5.9 deepen).
    /// Align jitter only when sync preflight passes; returns false when blocked (B5.9 deepen).
    /// True when pass jitter matches the expected slot for `frameIndex` (B5.9 deepen).
    bool isJitterSyncedTo(u32 frameIndex) const;
    /// Align jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    /// True when pass jitter index and monotonic counter match `frameIndex` (B5.9 deepen).
    /// Jitter sync preflight without mutating pass state (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;
    /// Advance jitter only when aligned to `frameIndex` (B5.9 deepen).
    bool advanceJitterIfAlignedToFrameIndex(u32 frameIndex);
    /// Classify why pass jitter sync to `frameIndex` would be rejected (B5.9 deepen).
    /// True when pass jitter can sync to `frameIndex` (B5.9 deepen).
    /// Sync jitter with reject-reason diagnostics; returns false when blocked (B5.9 deepen).
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr);
    /// True when pass jitter must resync before sampling NDC offsets for `frameIndex` (B5.9 deepen).
    bool needsJitterResync(u32 frameIndex) const;
    /// True when pass jitter may align to `frameIndex` (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    void invalidateHistory();
    /// Invalidate when `observedGeneration` differs from pass history epoch (B5.9 deepen).
    bool invalidateHistoryIfStale(u32 observedGeneration);
    void resize(u32 width, u32 height);
    bool matchesDimensions(u32 width, u32 height) const;
    /// True when the resolve request matches this pass viewport dimensions.
    bool viewportMatchesResolve(const TaaResolveDesc& desc) const;
    bool needsHistoryWarmup() const { return m_history.needsWarmup(); }
    /// Frames remaining before pass history may be temporally reused (B5.9 deepen).
    u32 warmupFramesRemaining() const;
    /// True when pass history is allocated but still awaiting first resolve (B5.9 deepen).
    bool historyWarmupRequired() const;
    /// True when pass history is warmed after init (B5.9 deepen).
    bool historyWarmupComplete() const;
    /// True when the next resolve would be the warm-up frame (B5.9 deepen).
    bool isWarmupResolveFrame() const;
    /// True when temporal history reuse is allowed for the observed invalidate epoch (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration) const;
    /// Preflight resolve blend weights; false when history is not ready or weights violate reuse policy (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc,
                               TaaResolveBlendPreflightRejectReason* reason = nullptr) const;
    /// Current history warm-up phase (B5.9 deepen).
    TaaHistoryWarmupPhase historyWarmupPhase() const;
    /// True when pass history is warmed (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupPhase* phase = nullptr) const;
    /// Frames remaining before temporal reuse is allowed — 0 when warmed (B5.9 deepen).
    u32 historyWarmupFramesRemaining() const;
    /// Classify why pass history warm-up is blocked (B5.9 deepen).
    TaaHistoryWarmupBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// True when pass history has completed warm-up (B5.9 deepen).
    /// True when pass history may begin temporal reuse for the observed epoch (B5.9 deepen).
    bool tryCanBeginTemporalReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// True when pass history is warmed and may be sampled (B5.9 deepen).
    bool canReuseHistory() const;
    /// True when pass history is ready, warmed, and generation matches for reuse (B5.9 deepen).
    bool historyReuseReady(u32 observedGeneration) const;
    /// Early-out when pass history temporal reuse should be skipped (B5.9 deepen).
    bool shouldSkipHistoryReuse(u32 observedGeneration) const;
    /// True when history blend is allowed on the next resolve (B5.9 deepen).
    bool historyBlendAllowed() const;
    /// True when pass jitter can produce NDC offsets for the configured viewport (B5.9 deepen).
    bool canProduceJitterNdc() const;
    /// Advance jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool advanceJitterIfReady();
    /// Advance jitter only when aligned to `expectedFrameIndex`; returns false when drifted (B5.9 deepen).
    bool advanceJitterIfAligned(u32 expectedFrameIndex);
    /// True when pass jitter monotonic counter and slot match `expectedFrameIndex` (B5.9 deepen).
    bool preflightJitterAlignment(u32 expectedFrameIndex) const;
    /// Advance jitter only when sequence and viewport are valid; returns false when blocked (B5.9 deepen).
    bool advanceJitterIfViewportReady();
    /// Expected blend weights for the next resolve (B5.9 deepen).
    TaaBlendWeights expectedResolveBlendWeights(const TaaResolveDesc& desc) const;
    /// True when resolve would sample warmed history this frame (B5.9 deepen).
    bool resolveWouldReuseHistory(const TaaResolveDesc& desc) const;
    /// Classify why pass history reuse is blocked (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryReuseBlock(u32 observedGeneration) const;
    /// True when pass history temporal reuse is allowed (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// True when expected resolve blend weights pass validation and reuse policy (B5.9 deepen).
    bool preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                      TaaResolveBlendRejectReason* reason = nullptr) const;
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// Early-out when resolve blend-weight preflight would reject (B5.9 deepen).
    bool shouldSkipResolveBlend(const TaaResolveDesc& desc) const;
    /// True when pass jitter can sync to `frameIndex` (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass jitter sync preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSync(u32 frameIndex) const;
    /// True when pass jitter can produce NDC offsets for the configured viewport (B5.9 deepen).
    bool preflightJitterNdc(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass NDC jitter preflight would reject (B5.9 deepen).
    bool shouldSkipJitterNdc() const;
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// True when pass history buffers are allocated and ready for resolve (B5.9 deepen).
    bool historyReadyForResolve() const;
    /// Early-out when pass history is not ready for resolve (B5.9 deepen).
    bool shouldSkipHistoryResolve() const;
    /// Early-out when resolve preflight would skip (B5.9 deepen).
    bool shouldSkipResolve(const TaaResolveDesc& desc) const;
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc, TaaResolveBlendRejectReason& reason) const;
    /// Classify why NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    /// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    /// Classify why jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// True when jitter can advance for the configured sequence (B5.9 deepen).
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when jitter advance preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAdvance() const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// True when jitter monotonic frame and slot match `frameIndex` (B5.9 deepen).
    /// True when pass jitter state matches the given monotonic frame counter (B5.9 deepen).
    bool isJitterSyncedToFrameIndex(u32 frameIndex) const;
    /// True when resolve request passes all preflight guards (B5.9 deepen).
    bool canResolveFrame(const TaaResolveDesc& desc) const;
    /// Stamp generation and return whether resolve can proceed (B5.9 deepen).
    bool prepareAndCanResolve(TaaResolveDesc& desc) const;
    /// True when the next resolve would sample prior history (B5.9 deepen).
    bool resolveWillReuseHistory(const TaaResolveDesc& desc) const;
    /// True when pass jitter slot and monotonic counter match a frame index (B5.9 deepen).
    TaaHistoryWarmupPreflight preflightHistoryWarmup() const;
    TaaHistoryReusePreflight preflightHistoryReuse(u32 observedGeneration) const;
    TaaHistoryReusePreflight preflightHistoryReuseForDesc(const TaaResolveDesc& desc) const;
    TaaJitterSyncPreflight preflightJitterSync(u32 frameIndex) const;
    TaaResolveBlendPreflight preflightResolveBlend(const TaaResolveDesc& desc) const;
    /// Monotonic frame counter owned by pass jitter (B5.9 deepen).
    u32 jitterMonotonicFrameIndex() const { return m_jitter.monotonicFrameIndex(); }
    /// True when pass jitter is aligned to a monotonic frame counter (B5.9 deepen).
    /// Preflight resolve blend weights without mutating history (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* out = nullptr) const;
    /// Classify why pass history reuse would be rejected (B5.9 deepen).
    TaaHistoryReuseRejectReason classifyHistoryReuseReject(u32 observedGeneration) const;
    /// Preflight resolve eligibility and blend weights without mutating history (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaResolveBlendPreflight* out = nullptr) const;
    /// True when pass jitter state matches `frameIndex` (B5.9 deepen).
    /// True when jitter may be aligned via `syncJitterToFrameIndex` (B5.9 deepen).
    bool canSyncJitterToFrameIndex() const;
    /// Preflight resolve plus blend-weight validation; optionally fills `weights` (B5.9 deepen).
    /// True when pass jitter monotonic frame and slot align with `frameIndex` (B5.9 deepen).
    /// True when pass history is warmed after the first successful resolve (B5.9 deepen).
    bool historyWarmupComplete() const;
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights = nullptr) const;
    /// True when history warm-up has completed (B5.9 deepen).
    /// Warm-up progress in [0, 1] for pass history (B5.9 deepen).
    f32 historyWarmupProgress() const;
    /// True when pass history is warmed and observed generation is current (B5.9 deepen).
    bool historyReuseReady(u32 observedGeneration) const;
    /// Classify why pass jitter sync is blocked (B5.9 deepen).
    TaaJitterSyncBlockReason classifyJitterSyncBlock() const;
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;
    /// True when resolve skip + blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                                   TaaResolveBlendRejectReason* blendReason = nullptr) const;
    /// True when last resolve stats blend weights match computed policy (B5.9 deepen).
    bool lastResolveStatsBlendConsistent(const TaaResolveDesc& desc) const;
    /// True when pass history is warmed for temporal contribution (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// Frames remaining before pass history may be reused — 0 when warmed (B5.9 deepen).
    u32 warmupFramesRemaining() const;
    /// True when pass history reuse is allowed for a resolve request (B5.9 deepen).
    bool preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                         TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// True when pass jitter is aligned to `frameIndex` (B5.9 deepen).
    /// True when resolve skip and blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                               TaaResolveBlendRejectReason* blendRejectReason = nullptr) const;
    /// Convenience wrapper — true when pass history reuse preflight would pass (B5.9 deepen).
    bool canPreflightHistoryReuse(u32 observedGeneration) const;
    /// Classify why pass history warm-up blocks temporal reuse (B5.9 deepen).
    TaaHistoryWarmupBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history is warmed for temporal reuse (B5.9 deepen).
    /// Convenience wrapper — true when pass history warm-up preflight would pass (B5.9 deepen).
    bool canPreflightHistoryWarmup() const;
    /// True when resolve may apply a non-zero history blend this frame (B5.9 deepen).
    bool temporalBlendReady(const TaaResolveDesc& desc) const;
    /// Convenience wrapper — true when pass blend-weight preflight would pass (B5.9 deepen).
    bool canPreflightResolveBlendWeights(const TaaResolveDesc& desc) const;
    /// Fill `out` only when blend-weight preflight passes (B5.9 deepen).
    bool tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& out,
    /// True when history reuse and blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                       TaaResolveTemporalRejectReason* reason = nullptr) const;
    /// Convenience wrapper — true when pass temporal-blend preflight would pass (B5.9 deepen).
    bool canPreflightResolveTemporalBlend(const TaaResolveDesc& desc) const;
    /// True when resolve request passes skip preflight guards (B5.9 deepen).
    /// Sanitize desc and return whether resolve can proceed (B5.9 deepen).
    bool preflightResolveFrame(const TaaResolveDesc& desc,
                               TaaResolveSkipReason* skipReason = nullptr,
    /// True when pass jitter monotonic counter or slot differs from `frameIndex` (B5.9 deepen).
    bool jitterNeedsResyncToFrameIndex(u32 frameIndex) const;
    /// True when pass jitter can align to `frameIndex` without drift (B5.9 deepen).
    /// Frames remaining before pass history may be temporally reused (B5.9 deepen).
    u32 historyWarmupFramesRemaining() const;
    /// True when pass history has completed warm-up (B5.9 deepen).
    /// History reuse preflight using resolve desc generation (B5.9 deepen).
    bool preflightHistoryReuseForDesc(const TaaResolveDesc& desc,
    /// Combined resolve skip + blend-weight preflight (B5.9 deepen).
    bool preflightResolveGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
    /// True when resolve temporal blend and history reuse preflights both pass (B5.9 deepen).
    bool resolveTemporalBlendAllowed(const TaaResolveDesc& desc) const;
    /// Combined resolve temporal-blend preflight with optional reject diagnostics (B5.9 deepen).
                                       TaaResolveBlendRejectReason* blendReason = nullptr,
                                       TaaHistoryReuseBlockReason* reuseReason = nullptr) const;
    /// Classify pass history warm-up state (B5.9 deepen).
    TaaHistoryWarmupState classifyHistoryWarmupState() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupState* state = nullptr) const;
    /// Classify resolve blend mode for the next resolve (B5.9 deepen).
    TaaResolveBlendMode classifyResolveBlendMode(const TaaResolveDesc& desc) const;
    /// True when pass jitter slot differs from the frame-index mapping (B5.9 deepen).
    bool jitterNeedsSyncToFrameIndex(u32 frameIndex) const;
    /// Combined resolve-frame preflight — skip check then blend-weight validation (B5.9 deepen).
    /// Preflight resolve skip and blend-weight guards for one resolve request (B5.9 deepen).
    bool preflightResolveDesc(const TaaResolveDesc& desc, TaaResolveDescPreflight* result = nullptr) const;
    /// True when resolve-desc history reuse preflight passes (B5.9 deepen).
    /// Preflight resolve skip, history reuse, and blend weights together (B5.9 deepen).
    bool preflightResolveTemporalAccumulation(const TaaResolveDesc& desc,
                                              TaaResolveTemporalPreflight* result = nullptr) const;
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveFramePreflight* out = nullptr) const;
    /// Expected blend weights when preflight passes; returns false on reject (B5.9 deepen).
    bool tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                        TaaResolveBlendRejectReason* reason = nullptr) const;
    u32 historyInvalidateGeneration() const { return m_history.invalidateGeneration(); }
    /// True when a consumer's observed generation differs from pass history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    /// True when history targets are warm enough for temporal reuse.
    bool canReadHistory() const { return m_history.canReadForResolve(); }
    /// Effective current-frame blend for the next resolve (1.0 while history is cold).
    /// True when history targets are warm enough for temporal reuse (B5.9 deepen).
    /// Effective current-frame blend for the next resolve (1.0 while history is cold) (B5.9 deepen).
    f32 effectiveBlendForNextResolve() const;
    /// Preflight resolve without mutating history (delegates to `TaaResolve::wouldSkip`).
    bool wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// Compute expected blend weights for a resolve request without mutating history (B5.9 deepen).
    TaaResolveBlendPreflight preflightResolveBlend(const TaaResolveDesc& desc) const;
    /// Resolve + blend preflight using pass history — optionally fills projected blend weights (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights = nullptr) const;
    /// Preflight history reuse for an observed invalidate epoch (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Preflight resolve blend weights for consistency with reuse policy (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc) const;
    /// Stamp `observed_history_generation` from pass history when still at the no-guard sentinel.
    void stampObservedHistoryGeneration(TaaResolveDesc& desc) const;
    /// Clamp params and stamp observed generation from pass history.
    void sanitizeResolveDesc(TaaResolveDesc& desc) const;
    /// True when `observedGeneration` matches the current history invalidate epoch.
    bool isObservedHistoryGenerationCurrent(u32 observedGeneration) const;
    /// True when resolve request passes all preflight guards (B5.9 deepen).
    bool canResolveFrame(const TaaResolveDesc& desc) const;
    /// Stamp generation and return whether resolve can proceed (B5.9 deepen).
    bool prepareAndCanResolve(TaaResolveDesc& desc) const;
    /// True when history is warm and generation guard passes for this resolve request.
    bool canReuseHistory(const TaaResolveDesc& desc) const;
    /// Effective current-frame blend for the next resolve (1.0 while history is cold or non-reusable).
    f32 effectiveBlendForNextResolve(const TaaResolveDesc& desc) const;
    /// True when pass jitter matches a monotonic frame counter (B5.9 deepen).
    bool isJitterSyncedToFrameIndex(u32 frameIndex) const;
    /// Preflight history reuse for the pass history epoch (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration,
                                TaaHistoryReuseRejectReason* reason = nullptr) const;
    /// Preflight resolve history-blend for the pass history state (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc,
                                TaaBlendPreflightRejectReason* reason = nullptr) const;

    bool resolveFrame(const TaaResolveDesc& desc, void* cudaStream = nullptr);

private:
    explicit TaaPass(const TaaPassDesc& desc);

    TaaPassDesc m_desc{};
    TaaPassStats m_stats{};
    TaaJitter m_jitter;
    TaaHistoryBuffer m_history;
    TaaResolve m_resolve;
};

void resetTaaPassGraphStorage();
void addTaaPassToGraph(RenderGraph& graph);

} // namespace fuse::renderer
