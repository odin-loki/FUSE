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

/// Composite temporal guard verdict for one frame (B5.9 deepen).
struct TaaPassTemporalGuardVerdict {
    bool jitterSyncOk = false;
    bool jitterNdcOk = false;
    bool historyWarmupComplete = false;
    bool historyReuseOk = false;
    bool resolveBlendOk = false;
    TaaJitterGuardRejectReason jitterReject = TaaJitterGuardRejectReason::None;
    TaaHistoryReuseBlockReason historyReject = TaaHistoryReuseBlockReason::None;
    TaaResolveBlendRejectReason blendReject = TaaResolveBlendRejectReason::None;
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
    /// NDC jitter with reject-reason diagnostics (B5.9 deepen).
    bool tryCurrentJitterNdcIfReady(fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason) const;
    /// Pixel jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool currentJitterPixelOffsetIfReady(fuse::math::Vec2& out) const;
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
    /// True when pass jitter is not aligned to `frameIndex` but could sync (B5.9 deepen).
    bool needsJitterSyncToFrameIndex(u32 frameIndex) const;
    /// Sync jitter only when misaligned; returns false when blocked (B5.9 deepen).
    bool syncJitterToFrameIndexIfMisaligned(u32 frameIndex);
    /// Sync jitter with reject-reason diagnostics (B5.9 deepen).
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason);
    /// Sync jitter with mandatory reject-reason output; returns false when blocked (B5.9 deepen).
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Sync jitter with mandatory reject-reason output when preflight rejects (B5.9 deepen).
    /// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Sync jitter only when preflight passes; fills reject reason on failure (B5.9 deepen).
    bool trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason);
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
    /// Sync jitter and produce NDC offset only when viewport and sequence are valid (B5.9 deepen).
    bool syncJitterToFrameIndexAndProduceNdcIfReady(u32 frameIndex, fuse::math::Vec2& out);
    /// True when pass jitter state is aligned to `frameIndex` (B5.9 deepen).
    bool preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen).
    bool canAdvanceJitter() const;
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    TaaJitterGuardRejectReason classifyJitterSyncReject(u32 frameIndex) const;
    /// Classify why pass NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    /// Classify why pass jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAdvance() const;
    /// NDC jitter for a monotonic frame counter only when viewport and sequence are valid (B5.9 deepen).
    bool ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const;
    /// Classify why pass jitter sync to a frame counter would be rejected (B5.9 deepen).
    void invalidateHistory();
    /// Invalidate when `observedGeneration` differs from pass history epoch (B5.9 deepen).
    /// Invalidate when `observedGeneration` differs from pass history epoch; returns true when invalidated.
    bool invalidateHistoryIfStale(u32 observedGeneration);
    void resize(u32 width, u32 height);
    bool matchesDimensions(u32 width, u32 height) const;
    /// True when the resolve request matches this pass viewport dimensions.
    bool viewportMatchesResolve(const TaaResolveDesc& desc) const;
    bool needsHistoryWarmup() const { return m_history.needsWarmup(); }
    /// True when pass history is warmed and may be temporally reused (B5.9 deepen).
    /// True when pass history is warmed and no longer needs a warm-up frame (B5.9 deepen).
    /// True when pass history is warmed for temporal reuse (B5.9 deepen).
    bool historyWarmupComplete() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// True when pass history is allocated and warmed for temporal reuse (B5.9 deepen).
    /// Classify pass history warm-up lifecycle (B5.9 deepen).
    TaaHistoryWarmupState classifyHistoryWarmupState() const;
    bool preflightHistoryWarmup(TaaHistoryWarmupState* state = nullptr) const;
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
    /// True when pass history warm-up is complete (B5.9 deepen).
    /// Classify why pass history warm-up is blocked (B5.9 deepen).
    TaaHistoryWarmupBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// True when pass history has completed warm-up (B5.9 deepen).
    /// True when pass history may begin temporal reuse for the observed epoch (B5.9 deepen).
    bool tryCanBeginTemporalReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Frames remaining before temporal reuse — 0 when warmed (B5.9 deepen).
    /// True when pass history buffers are allocated and ready for resolve (B5.9 deepen).
    bool historyReadyForResolve() const;
    /// True when pass history still needs its first resolve frame (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason = nullptr) const;
    /// Early-out when pass history warm-up is complete or not ready (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    /// Early-out when pass history warm-up is not complete (B5.9 deepen).
    /// Early-out when pass history warm-up preflight would reject (B5.9 deepen).
    /// True when pass history is warmed and may be sampled (B5.9 deepen).
    bool canReuseHistory() const;
    /// True when pass history has completed warm-up (B5.9 deepen).
    bool isHistoryWarm() const;
    /// True when pass history is ready, warmed, and generation matches for reuse (B5.9 deepen).
    bool historyReuseReady(u32 observedGeneration) const;
    /// Early-out when pass history temporal reuse should be skipped (B5.9 deepen).
    bool shouldSkipHistoryReuse(u32 observedGeneration) const;
    /// Early-out when pass history warm-up is incomplete (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// Classify why pass history warm-up is blocked (B5.9 deepen).
    TaaHistoryWarmupBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// Alias for `shouldSkipHistoryReuse` — same ordering as reuse preflight (B5.9 deepen).
    bool wouldSkipHistoryReuse(u32 observedGeneration) const;
    bool preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason = nullptr) const;
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    /// Classify history warm-up phase for pass history (B5.9 deepen).
    TaaHistoryWarmupPhase historyWarmupPhase() const;
    /// Early-out when pass history reuse should be skipped for a resolve request (B5.9 deepen).
    bool shouldSkipHistoryReuseForResolve(const TaaResolveDesc& desc) const;
    /// True when pass history temporal reuse is allowed for a resolve request (B5.9 deepen).
    bool preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                         TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Invalidate when observed generation differs from pass history epoch (B5.9 deepen).
    bool invalidateHistoryIfStale(u32 observedGeneration);
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
    /// Advance jitter with reject-reason diagnostics (B5.9 deepen).
    bool tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason);
    /// True when pass jitter can advance (B5.9 deepen).
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen).
    /// Advance jitter with mandatory reject-reason output; returns false when blocked (B5.9 deepen).
    bool tryAdvanceJitter(TaaJitterGuardRejectReason& reason);
    /// Early-out when jitter advance preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAdvance() const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// Advance jitter with mandatory reject-reason output when preflight rejects (B5.9 deepen).
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen).
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Expected blend weights for the next resolve (B5.9 deepen).
    TaaBlendWeights expectedResolveBlendWeights(const TaaResolveDesc& desc) const;
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                       TaaResolveBlendRejectReason& reason) const;
    /// True when resolve would sample warmed history this frame (B5.9 deepen).
    bool resolveWouldReuseHistory(const TaaResolveDesc& desc) const;
    /// Classify why pass history reuse is blocked (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryReuseBlock(u32 observedGeneration) const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// True when pass history temporal reuse is allowed (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const;
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// True when history is warmed and temporal reuse is allowed (B5.9 deepen).
    bool preflightHistoryWarmupAndReuse(u32 observedGeneration,
                                        TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Classify why expected resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc, TaaResolveBlendRejectReason& reason) const;
    /// Compute expected resolve blend weights with mandatory reject-reason output (B5.9 deepen).
    /// Classify why pass history warm-up is blocked (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryWarmupBlock() const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Classify why pass jitter sync to a frame counter would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Classify why pass resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// True when expected resolve blend weights pass validation and reuse policy (B5.9 deepen).
    bool preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                      TaaResolveBlendRejectReason* reason = nullptr) const;
    /// True when pass history is warmed and ready for temporal reuse (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// History warmup preflight with mandatory reject-reason output (B5.9 deepen).
    /// Classify why pass history warm-up preflight would reject (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    bool tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const;
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    /// Compute expected resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// True when pass history buffers are allocated and ready for resolve (B5.9 deepen).
    /// Combined history warmup + reuse preflight (B5.9 deepen).
    bool preflightHistoryTemporal(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Early-out when pass history warmup or reuse preflight would reject (B5.9 deepen).
    bool shouldSkipHistoryTemporal(u32 observedGeneration) const;
    /// Combined resolve + blend-weight preflight (B5.9 deepen).
    bool preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                                   TaaResolveBlendRejectReason* blendReason = nullptr) const;
    /// Early-out when resolve or blend-weight preflight would reject (B5.9 deepen).
    bool shouldSkipResolveWithBlend(const TaaResolveDesc& desc) const;
    /// History reuse preflight with mandatory block-reason output (B5.9 deepen).
    /// History resolve-readiness preflight with mandatory block-reason output (B5.9 deepen).
    /// Compute resolve blend weights with mandatory reject-reason output (B5.9 deepen).
    bool tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// True when pass history buffers are warmed and temporal reuse is allowed (B5.9 deepen).
    /// Early-out when pass history warmup/reuse preflight would reject (B5.9 deepen).
    bool shouldSkipHistoryWarmupAndReuse(u32 observedGeneration) const;
    bool tryComputeExpectedResolveBlendWeights(const TaaResolveDesc& desc,
                                               TaaBlendWeights& outWeights,
    /// Classify why pass jitter sync would be rejected (B5.9 deepen).
    /// Classify why pass NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    /// Classify why pass jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// Classify why pass resolve would skip (B5.9 deepen).
    /// Pass history reuse preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass history resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    /// Compute pass resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// Classify why resolve would skip for the pass history state (B5.9 deepen).
    /// Classify why pass NDC jitter would be rejected (B5.9 deepen).
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    /// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
    /// Classify why resolve would skip for this pass (B5.9 deepen).
    /// Classify why pass history warm-up is blocked (B5.9 deepen).
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc, TaaResolveBlendRejectReason& reason) const;
    /// Classify why expected resolve blend weights would be rejected (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                       TaaResolveBlendRejectReason& reason) const;
    /// Early-out when resolve blend-weight preflight would reject (B5.9 deepen).
    bool shouldSkipResolveBlend(const TaaResolveDesc& desc) const;
    /// Early-out when resolve preflight would bail (B5.9 deepen).
    bool shouldSkipResolve(const TaaResolveDesc& desc) const;
    /// True when resolve and blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveWithBlend(const TaaResolveDesc& desc,
                                   TaaResolveWithBlendRejectReason* reason = nullptr) const;
    /// Early-out when combined resolve+blend preflight would reject (B5.9 deepen).
    /// True when combined history-reuse and blend-weight preflights pass (B5.9 deepen).
    bool preflightResolveTemporal(const TaaResolveDesc& desc, u32 observedGeneration,
                                  TaaResolveTemporalRejectReason* reason = nullptr) const;
    /// Early-out when combined resolve temporal preflight would reject (B5.9 deepen).
    bool shouldSkipResolveTemporal(const TaaResolveDesc& desc, u32 observedGeneration) const;
    /// True when resolve may apply a non-zero temporal history blend (B5.9 deepen).
    bool preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                       TaaHistoryReuseBlockReason* reuseReason = nullptr,
    /// Temporal blend preflight with mandatory reject-reason outputs (B5.9 deepen).
    bool tryPreflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                          TaaHistoryReuseBlockReason& reuseReason,
                                          TaaResolveBlendRejectReason& blendReason) const;
    /// Early-out when temporal history blend preflight would reject (B5.9 deepen).
    /// True when combined resolve temporal-blend preflight passes (B5.9 deepen).
                                       TaaResolveTemporalBlendRejectReason* reason = nullptr) const;
    /// Early-out when combined resolve temporal-blend preflight would reject (B5.9 deepen).
    bool shouldSkipResolveTemporalBlend(const TaaResolveDesc& desc) const;
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen).
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// True when pass jitter can advance (B5.9 deepen).
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAdvance() const;
    /// Classify why pass jitter sync to a frame counter would be rejected (B5.9 deepen).
    /// Classify why pass jitter sync to `frameIndex` would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject(u32 frameIndex) const;
    /// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
    /// Classify why NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// True when pass jitter can sync to `frameIndex` (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// True when history reuse and resolve blend-weight preflights both pass (B5.9 deepen).
    bool preflightTemporalBlend(const TaaResolveDesc& desc,
    /// Early-out when temporal blend preflight would reject reuse or blend weights (B5.9 deepen).
    bool shouldSkipTemporalBlend(const TaaResolveDesc& desc) const;
    /// True when pass history has completed warm-up (B5.9 deepen).
    bool isHistoryWarmed() const;
    bool resolveBlendReady(const TaaResolveDesc& desc) const;
    /// True when pass history is resolve-ready and temporal reuse is allowed (B5.9 deepen).
    /// History temporal preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryTemporal(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// Early-out when pass history temporal preflight would reject (B5.9 deepen).
    /// True when resolve blend and history-reuse policy align for `desc` (B5.9 deepen).
    bool preflightResolveTemporal(const TaaResolveDesc& desc,
                                  TaaResolveBlendRejectReason* blendReason = nullptr,
                                  TaaHistoryReuseBlockReason* reuseReason = nullptr) const;
    /// Sync jitter only when preflight passes; returns false when blocked (B5.9 deepen).
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason);
    /// Pass jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    /// Sync jitter when preflight passes; returns false when blocked (B5.9 deepen).
    bool trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterGuardRejectReason& reason);
    /// Classify why pass history is not ready for resolve (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryResolveBlock() const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Classify why pass jitter sync would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass jitter sync preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSync(u32 frameIndex) const;
    /// Sync jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    /// Advance jitter only when the sequence is valid; returns false when blocked (B5.9 deepen).
    bool tryAdvanceJitterIfReady(TaaJitterGuardRejectReason& reason);
    /// True when pass jitter can sync to `frameIndex` but is not yet aligned (B5.9 deepen).
    bool jitterNeedsResync(u32 frameIndex) const;
    /// Early-out when pass jitter should resync to `frameIndex` (B5.9 deepen).
    bool shouldResyncJitter(u32 frameIndex) const;
    /// True when jitter slot index is within the active sequence (B5.9 deepen).
    bool preflightJitterSlot(u32 slot, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter slot preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSlot(u32 slot) const;
    /// Jitter alignment preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// True when pass jitter is aligned to `frameIndex` (B5.9 deepen).
    bool preflightJitterAligned(u32 frameIndex, TaaJitterAlignmentRejectReason* reason = nullptr) const;
    /// True when pass jitter state matches `frameIndex` (B5.9 deepen).
    bool preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter alignment preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAlignment(u32 frameIndex) const;
    /// True when pass jitter can sync and produce NDC offsets for the configured viewport (B5.9 deepen).
    bool preflightJitterSyncAndNdc(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when combined pass jitter sync + NDC preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSyncAndNdc(u32 frameIndex) const;
    /// True when pass jitter monotonic counter and slot match `frameIndex` (B5.9 deepen).
    /// True when history is warmed, reuse is allowed, and blend preflight passes (B5.9 deepen).
    bool preflightTemporalResolve(const TaaResolveDesc& desc,
    /// Classify why NDC jitter production would be rejected (B5.9 deepen).
    /// True when pass history buffers are ready for resolve preflight (B5.9 deepen).
    bool preflightHistoryReadyForResolve(TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// NDC jitter for `frameIndex` only when viewport and sequence are valid (B5.9 deepen).
    bool ndcOffsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const;
    /// Classify why pass jitter sync would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// True when pass jitter can produce NDC offsets for the configured viewport (B5.9 deepen).
    bool preflightJitterNdc(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// True when jitter sync and NDC preflights both pass (B5.9 deepen).
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    /// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
    /// Classify why pass NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    /// Classify why pass jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass NDC jitter preflight would reject (B5.9 deepen).
    bool shouldSkipJitterNdc() const;
    /// True when pass jitter can advance for the active sequence (B5.9 deepen).
    /// True when pass jitter can advance without blocking (B5.9 deepen).
    /// Advance jitter with reject-reason diagnostics; returns false when blocked (B5.9 deepen).
    /// Sync jitter with reject-reason diagnostics; returns false when blocked (B5.9 deepen).
    /// True when pass jitter sequence can advance (B5.9 deepen).
    /// Compute expected blend weights with reject-reason diagnostics (B5.9 deepen).
    /// Full resolve preflight with mandatory skip-reason output (B5.9 deepen).
    /// Evaluate jitter sync, history warmup/reuse, and resolve-blend preflights (B5.9 deepen).
    void evaluateTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc, u32 observedGeneration,
                                TaaPassTemporalGuardVerdict& verdict) const;
    /// True when all temporal guards pass (B5.9 deepen).
    bool preflightTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc, u32 observedGeneration,
                                 TaaPassTemporalGuardVerdict* verdict = nullptr) const;
    /// Early-out when any temporal guard would reject (B5.9 deepen).
    bool shouldSkipTemporalGuards(u32 frameIndex, const TaaResolveDesc& desc,
                                  u32 observedGeneration) const;
    /// Halton pixel offset for a frame counter when the sequence is valid (B5.9 deepen).
    bool offsetForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const;
    /// NDC jitter for a frame counter when viewport and sequence are valid (B5.9 deepen).
    /// Combined jitter sync + NDC preflight for a frame (B5.9 deepen).
    bool preflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter sync or NDC preflight would reject (B5.9 deepen).
    bool shouldSkipJitterFrame(u32 frameIndex) const;
    /// True when pass jitter can advance (B5.9 deepen).
    /// NDC jitter for a frame counter only when viewport and sequence are valid (B5.9 deepen).
    bool ndcJitterForFrameIndexIfReady(u32 frameIndex, fuse::math::Vec2& out) const;
    /// True when jitter can sync to `frameIndex` and produce NDC for the pass viewport (B5.9 deepen).
    /// Early-out when pass jitter sync+NDC preflight would reject (B5.9 deepen).
    /// True when pass jitter sync and NDC production both pass for `frameIndex` (B5.9 deepen).
    /// Jitter frame preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass jitter frame preflight would reject (B5.9 deepen).
    /// Advance jitter with mandatory reject-reason output; returns false when blocked (B5.9 deepen).
    bool tryAdvanceJitter(TaaJitterGuardRejectReason& reason);
    bool canAdvanceJitter() const;
    bool canSyncJitterToFrameIndex(u32 frameIndex) const;
    /// Advance jitter only when preflight passes; returns false when blocked (B5.9 deepen).
    /// Pass jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Classify why pass NDC jitter would be rejected (B5.9 deepen).
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAdvance() const;
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen).
    /// Classify why pass jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    /// Classify why pass NDC jitter would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    bool historyWarmupComplete() const;
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const;
    bool preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason = nullptr) const;
    bool tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const;
    /// True when pass history warm-up completion preflight passes (B5.9 deepen).
    bool preflightHistoryWarmupComplete(TaaHistoryWarmupRejectReason* reason = nullptr) const;
    /// Early-out when pass history warm-up is not yet complete (B5.9 deepen).
    bool shouldSkipHistoryWarmupComplete() const;
    /// Classify why pass history warm-up is not yet satisfied (B5.9 deepen).
    TaaHistoryReuseBlockReason classifyHistoryWarmupBlock() const;
    /// True when pass history warm-up is satisfied (B5.9 deepen).
    bool preflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason* reason = nullptr) const;
    bool tryPreflightHistoryWarmupSatisfied(TaaHistoryReuseBlockReason& reason) const;
    /// True when pass history is ready, warmed, and generation matches for temporal sampling (B5.9 deepen).
    bool preflightHistoryTemporalSample(u32 observedGeneration,
                                        TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Early-out when pass history temporal sampling should be skipped (B5.9 deepen).
    bool shouldSkipHistoryTemporalSample(u32 observedGeneration) const;
    /// True when pass history buffers are allocated and ready for resolve (B5.9 deepen).
    bool historyReadyForResolve() const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Early-out when pass history is not ready for resolve (B5.9 deepen).
    bool shouldSkipHistoryResolve() const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Preflight resolve without mutating history (B5.9 deepen).
    bool preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Classify why resolve would skip for the pass history state (B5.9 deepen).
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// Classify why resolve would skip (B5.9 deepen).
    /// Classify why resolve would skip for this pass (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// Early-out when resolve preflight would skip (B5.9 deepen).
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
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
    /// True when pass jitter is not aligned to `frameIndex` (B5.9 deepen).
    /// Diagnose pass jitter sync preflight; false when sync would be blocked (B5.9 deepen).
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason& outReason);
    /// True when pass history temporal reuse would be skipped (B5.9 deepen).
    bool shouldSkipHistoryReuse(u32 observedGeneration) const;
    /// True when resolve may sample warmed pass history this frame (B5.9 deepen).
    bool canSampleHistoryForResolve(const TaaResolveDesc& desc) const;
    /// Diagnose resolve-context pass history reuse; false when sampling would be blocked (B5.9 deepen).
    bool tryPreflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                            TaaHistoryReuseBlockReason& outReason) const;
    /// True when resolve would skip history blend on the next frame (B5.9 deepen).
    bool shouldSkipHistoryBlendAtResolve(const TaaResolveDesc& desc) const;
    /// True when resolve may apply a non-zero history blend weight (B5.9 deepen).
    bool canApplyHistoryBlendAtResolve(const TaaResolveDesc& desc) const;
    /// True when pass history is warmed and no longer needs a warm-up frame (B5.9 deepen).
    /// True when pass history warm-up preflight passes (B5.9 deepen).
    /// True when pass jitter can sync to a monotonic frame counter (B5.9 deepen).
    /// Sync jitter only when viewport and sequence preflight pass (B5.9 deepen).
    bool syncJitterToFrameIndexIfViewportReady(u32 frameIndex);
    /// History reuse preflight using resolve desc observed generation (B5.9 deepen).
    bool preflightResolveHistoryReuse(const TaaResolveDesc& desc,
    /// Resolve-desc-aware history reuse preflight (B5.9 deepen).
    /// True when blend preflight passes and reuse preflight passes when history blend would apply (B5.9 deepen).
    bool preflightResolveTemporal(const TaaResolveDesc& desc,
    /// Early-out when pass history reuse would be blocked (B5.9 deepen).
    bool wouldSkipHistoryReuse(u32 observedGeneration) const;
    /// Diagnose pass history reuse preflight with a required reject reason (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& outReason) const;
    /// Early-out when pass resolve blend preflight would reject (B5.9 deepen).
    bool wouldRejectResolveBlendWeights(const TaaResolveDesc& desc) const;
    /// Diagnose pass resolve blend preflight with a required reject reason (B5.9 deepen).
                                         TaaResolveBlendRejectReason& outReason) const;
    /// Early-out when pass jitter sync would be blocked (B5.9 deepen).
    bool wouldSkipJitterSync(u32 frameIndex) const;
    /// Sync pass jitter with required reject-reason diagnostics (B5.9 deepen).
    bool trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason& outReason);
    bool jitterSyncReady(u32 frameIndex) const;
    bool jitterNdcReady() const;
    bool preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason = nullptr) const;
    /// Early-out when pass history warm-up should be skipped (B5.9 deepen).
    /// Early-out when pass jitter NDC preflight would reject (B5.9 deepen).
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// True when resolve blend weights pass validation and reuse policy (B5.9 deepen).
    /// True when pass jitter can produce NDC offsets (B5.9 deepen).
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    /// Early-out when resolve skip or blend-weight preflight would reject (B5.9 deepen).
    bool shouldSkipResolveFrame(const TaaResolveDesc& desc) const;
    /// True when history warm-up is complete (B5.9 deepen).
    /// Alias for `shouldSkipResolveBlend` — same ordering as blend preflight (B5.9 deepen).
    bool wouldSkipResolveBlend(const TaaResolveDesc& desc) const;
    /// Early-out when pass jitter NDC production would be blocked (B5.9 deepen).
    bool resolveBlendWeightsReady(const TaaResolveDesc& desc) const;
    /// Early-out when resolve would apply a non-zero history blend weight (B5.9 deepen).
    bool shouldSkipResolveHistoryBlend(const TaaResolveDesc& desc) const;
    /// Early-out when pass jitter sync to `frameIndex` would be rejected (B5.9 deepen).
    /// Early-out when pass jitter NDC production would be rejected (B5.9 deepen).
    /// True when pass jitter can advance (B5.9 deepen).
    /// Early-out when pass jitter advance would be rejected (B5.9 deepen).
    /// Early-out when pass jitter sync would be rejected (B5.9 deepen).
    /// Early-out when pass NDC jitter production would be rejected (B5.9 deepen).
    /// True when pass jitter NDC production is ready (B5.9 deepen).
    /// Current pass history warm-up lifecycle state (B5.9 deepen).
    TaaHistoryWarmupState historyWarmupState() const;
    /// True when combined resolve temporal preflight passes (B5.9 deepen).
    /// Early-out when combined resolve temporal preflight would reject (B5.9 deepen).
    bool shouldSkipResolveTemporal(const TaaResolveDesc& desc) const;
    /// True when pass history is warmed for temporal accumulation (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Early-out when resolve would bail before history update (B5.9 deepen).
    /// True when expected resolve blend weights are ready (B5.9 deepen).
    /// Compute expected resolve blend weights only when preflight passes (B5.9 deepen).
    bool computeResolveBlendWeightsIfReady(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    bool preflightResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration,
                                       TaaResolveReuseBlendRejectReason* reason = nullptr) const;
    /// Early-out when resolve reuse-and-blend preflight would reject (B5.9 deepen).
    bool shouldSkipResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration) const;
    /// Invalidate when `observedGeneration` differs from pass history epoch; returns true when invalidated.
    bool preflightTemporalResolve(const TaaResolveDesc& desc,
                                  TaaTemporalGuardRejectReason* reason = nullptr) const;
    /// Early-out when temporal resolve preflight would reject (B5.9 deepen).
    bool shouldSkipTemporalResolve(const TaaResolveDesc& desc) const;
    /// Invalidate history when `observedGeneration` differs from the current epoch (B5.9 deepen).
    bool invalidateHistoryIfStale(u32 observedGeneration);
    /// Early-out when pass jitter sync would be rejected (B5.9 deepen follow-up).
    /// True when pass jitter can produce NDC offsets for the configured viewport (B5.9 deepen follow-up).
    /// Early-out when pass jitter NDC production would be rejected (B5.9 deepen follow-up).
    /// Populate history warm-up diagnostics without mutating pass state (B5.9 deepen follow-up).
    /// Early-out when pass history still needs warm-up (B5.9 deepen follow-up).
    /// Populate resolve blend diagnostics without mutating pass state (B5.9 deepen follow-up).
    /// True when pass jitter must resync to `frameIndex` before projection (B5.9 deepen).
    bool jitterNeedsResync(u32 frameIndex) const;
    /// Combined resolve + blend-weight preflight (B5.9 deepen).
    /// Early-out when combined resolve frame preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSync() const;
    /// Compute expected resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// True when history reuse and resolve blend preflights both pass (B5.9 deepen).
    bool preflightTemporalResolveGuards(const TaaResolveDesc& desc, u32 observedGeneration,
    /// Early-out when temporal resolve guards would reject blend preflight (B5.9 deepen).
    bool shouldSkipTemporalResolveGuards(const TaaResolveDesc& desc, u32 observedGeneration) const;
    /// True when pass jitter can produce NDC offsets for viewport (B5.9 deepen).
    bool preflightJitterNdc(u32 width, u32 height, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// True when pass jitter can produce NDC offsets for configured viewport (B5.9 deepen).
    bool preflightJitterNdcIfReady(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass history warm-up preflight would reject (B5.9 deepen).
    bool shouldSkipHistoryWarmup() const;
    /// Early-out when combined resolve-frame preflight would reject (B5.9 deepen).
    /// Combined jitter sync + NDC preflight for a monotonic frame counter (B5.9 deepen).
    TaaJitterFramePreflight preflightJitterFrame(u32 frameIndex) const;
    /// Combined warm-up + reuse preflight for pass history (B5.9 deepen).
    TaaHistoryWarmupPreflight preflightHistoryWarmup(u32 observedGeneration) const;
    /// Combined resolve blend-weight preflight for the next resolve (B5.9 deepen).
    TaaResolveBlendPreflight preflightResolveBlendFrame(const TaaResolveDesc& desc) const;
    /// Combined history warm-up + resolve blend guard preflight (B5.9 deepen).
    TaaFrameGuardPreflight preflightFrameGuards(const TaaResolveDesc& desc, u32 observedGeneration) const;
    bool isHistoryWarmupComplete() const;
    /// Early-out when pass history still needs warm-up (B5.9 deepen).
    /// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
                                         TaaResolveBlendRejectReason& reason) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// Early-out when resolve preflight would bail before history update (B5.9 deepen).
    bool shouldSkipResolve(const TaaResolveDesc& desc) const;
    bool isWarmupComplete() const;
    bool shouldSkipWarmup() const;
    /// Combined history-reuse + blend-weight preflight for temporal resolve (B5.9 deepen).
    bool preflightTemporalResolve(const TaaResolveDesc& desc, TaaHistoryReuseBlockReason* reuseReason = nullptr,
    /// Early-out when combined temporal resolve preflight would reject (B5.9 deepen).
    /// Early-out when pass jitter sync should be skipped (B5.9 deepen).
    /// Early-out when pass jitter NDC production should be skipped (B5.9 deepen).
    bool preflightResolveFrameGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
    /// Early-out when resolve frame guards would reject (B5.9 deepen).
    bool shouldSkipResolveFrameGuards(const TaaResolveDesc& desc) const;
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen).
    /// Early-out when pass history warm-up blocks temporal reuse (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
    /// True when resolve preflight and blend-weight preflight both pass (B5.9 deepen).
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                               TaaResolveBlendRejectReason* blendReject = nullptr) const;
    /// Resolve-frame preflight with mandatory skip/blend reject outputs (B5.9 deepen).
    bool tryPreflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
                                  TaaResolveBlendRejectReason& blendReject) const;
    /// Early-out when resolve preflight or blend-weight preflight would reject (B5.9 deepen).
    bool shouldSkipResolveFrame(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// True when resolve skip and blend-weight preflights both pass (B5.9 deepen).
                               TaaResolveBlendRejectReason* blendReason = nullptr) const;
    /// Combined resolve-frame preflight with mandatory reject-reason outputs (B5.9 deepen).
                                  TaaResolveBlendRejectReason& blendReason) const;
    /// Early-out when resolve skip or blend-weight preflight would reject (B5.9 deepen).
    /// True when pass jitter slot/monotonic state matches `frameIndex` (B5.9 deepen).
    bool preflightJitterAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter alignment preflight would reject (B5.9 deepen).
    bool shouldSkipJitterAlignment(u32 frameIndex) const;
    /// True when pass jitter must resync before sampling offsets for `frameIndex` (B5.9 deepen).
    bool jitterNeedsSyncToFrameIndex(u32 frameIndex) const;
    /// Classify pass history warm-up lifecycle state (B5.9 deepen).
    TaaHistoryWarmupState classifyHistoryWarmupState() const;
    /// True when pass history warm-up is complete (B5.9 deepen).
    bool preflightHistoryWarmupComplete(TaaHistoryWarmupState* state = nullptr) const;
    /// Early-out when pass history warm-up is not complete (B5.9 deepen).
    bool shouldSkipHistoryWarmupComplete() const;
    /// True when pass history is warmed and generation matches for temporal blending (B5.9 deepen).
    bool preflightHistoryForTemporalBlend(u32 observedGeneration,
                                          TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Early-out when pass temporal history blending should be skipped (B5.9 deepen).
    bool shouldSkipHistoryForTemporalBlend(u32 observedGeneration) const;
    /// True when resolve and blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveWithBlendWeights(const TaaResolveDesc& desc,
                                          TaaResolveSkipReason* skipReason = nullptr,
    /// Early-out when combined resolve + blend preflight would reject (B5.9 deepen).
    bool shouldSkipResolveWithBlendWeights(const TaaResolveDesc& desc) const;
    /// History warm-up preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryWarmup(TaaHistoryReuseBlockReason& reason) const;
    bool historyWarmupComplete() const;
    /// Resolve blend policy preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendPolicy(const TaaResolveDesc& desc,
                                        TaaResolveBlendRejectReason& reason) const;
    /// Early-out when resolve blend policy preflight would reject (B5.9 deepen).
    bool shouldSkipResolveBlendPolicy(const TaaResolveDesc& desc) const;
    /// Combined resolve + blend preflight with mandatory reject-reason outputs (B5.9 deepen).
    bool tryPreflightResolveCombined(const TaaResolveDesc& desc, TaaResolveSkipReason& skipReason,
    /// Early-out when combined resolve or blend preflight would reject (B5.9 deepen).
    bool shouldSkipResolveCombined(const TaaResolveDesc& desc) const;
    /// Jitter sync alignment preflight with mandatory reject-reason output (B5.9 deepen).
    bool preflightJitterSyncAlignment(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter sync slot alignment preflight would reject (B5.9 deepen).
    bool shouldSkipJitterSyncAlignment(u32 frameIndex) const;
    /// True when pass jitter slot matches the Halton index for `frameIndex` (B5.9 deepen).
    bool jitterSlotAlignedToFrameIndex(u32 frameIndex) const;
    bool shouldSkipJitterAdvance() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterAdvance(TaaJitterGuardRejectReason& reason) const;
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc,
    /// Resolve-frame preflight with mandatory skip/blend reject-reason output (B5.9 deepen).
    /// Early-out when resolve-frame preflight would skip or reject blend weights (B5.9 deepen).
    /// Classify why resolve would skip (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Compute resolve blend weights with mandatory reject-reason output (B5.9 deepen).
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Jitter NDC preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterNdc(TaaJitterGuardRejectReason& reason) const;
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen).
    bool preflightJitterAdvance(TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Classify why pass jitter sync would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Classify why pass NDC jitter production would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterNdcReject() const;
    /// Classify why pass jitter advance would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterAdvanceReject() const;
    /// Classify why pass resolve blend weights would be rejected (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc, TaaResolveBlendRejectReason& reason) const;
    /// Compute expected resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// Classify why resolve would skip for the pass history state (B5.9 deepen).
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// Jitter advance preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// True when pass jitter can advance for the configured sequence (B5.9 deepen follow-up).
    /// Early-out when pass jitter advance preflight would reject (B5.9 deepen follow-up).
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen follow-up).
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen follow-up).
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen follow-up).
    /// Combined resolve-frame preflight — resolve skip plus blend-weight guards (B5.9 deepen follow-up).
    /// Early-out when combined resolve-frame preflight would reject (B5.9 deepen follow-up).
    /// NDC jitter preflight with mandatory reject-reason output (B5.9 deepen).
    /// True when pass jitter sequence can advance (B5.9 deepen).
    bool canAdvanceJitter() const;
    /// True when pass jitter sync and NDC preflights pass for `frameIndex` (B5.9 deepen).
    bool preflightJitterFrame(u32 frameIndex, TaaJitterGuardRejectReason* reason = nullptr) const;
    /// Early-out when pass jitter frame preflight would reject (B5.9 deepen).
    bool shouldSkipJitterFrame(u32 frameIndex) const;
    /// True when pass history buffers are ready for resolve (B5.9 deepen).
    bool preflightHistoryFrame(TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Early-out when pass history is not ready for resolve (B5.9 deepen).
    bool shouldSkipHistoryFrame() const;
    /// True when jitter, history, and resolve-blend preflights pass for one frame (B5.9 deepen).
    bool preflightResolveFrameGuards(const TaaResolveDesc& desc, u32 frameIndex, u32 observedGeneration,
                                       TaaResolveSkipReason* reason = nullptr) const;
    /// Early-out when resolve frame guard preflight would reject (B5.9 deepen).
    bool shouldSkipResolveFrameGuards(const TaaResolveDesc& desc, u32 frameIndex,
                                      u32 observedGeneration) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// Classify why resolve would skip for this pass (B5.9 deepen).
    /// Classify why pass resolve would skip (B5.9 deepen).
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    /// Classify why pass resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    /// Classify why pass jitter sync is blocked (B5.9 deepen).
    /// Pass jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass history reuse preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass history resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    /// Pass resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    /// Compute pass resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    /// Pass resolve preflight with mandatory skip-reason output (B5.9 deepen).
    u32 historyInvalidateGeneration() const { return m_history.invalidateGeneration(); }
    /// True when a consumer's observed generation differs from pass history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    /// True when history targets are warm enough for temporal reuse.
    bool canReadHistory() const { return m_history.canReadForResolve(); }
    /// Effective current-frame blend for the next resolve (1.0 while history is cold).
    /// True when history targets are warm enough for temporal reuse (B5.9 deepen).
    /// Effective current-frame blend for the next resolve (1.0 while history is cold) (B5.9 deepen).
    f32 effectiveBlendForNextResolve() const;
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    /// Classify why resolve would skip for the pass history state (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// Preflight resolve without mutating history (B5.9 deepen).
    bool preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Classify why resolve would skip (B5.9 deepen).
    /// Classify why resolve would skip for this pass (B5.9 deepen).
    /// Preflight resolve without mutating history (delegates to `TaaResolve::wouldSkip`).
    bool wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Compute expected blend weights for a resolve request without mutating history (B5.9 deepen).
    TaaResolveBlendPreflight preflightResolveBlend(const TaaResolveDesc& desc) const;
    /// Resolve + blend preflight using pass history — optionally fills projected blend weights (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights = nullptr) const;
    /// Preflight history reuse for an observed invalidate epoch (B5.9 deepen).
    bool preflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// Preflight resolve blend weights for consistency with reuse policy (B5.9 deepen).
    bool preflightResolveBlend(const TaaResolveDesc& desc) const;
    /// Preflight resolve without mutating history — returns true when resolve would proceed (B5.9 deepen).
    bool preflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Classify why resolve would skip (B5.9 deepen).
    /// Resolve preflight without mutating history — returns true when resolve would proceed (B5.9 deepen).
    /// Classify why resolve would skip for this pass (B5.9 deepen).
    /// Pass resolve preflight with mandatory skip-reason output (B5.9 deepen).
    /// True when resolve preflight passes without mutating history (B5.9 deepen).
    /// Classify why pass resolve would skip (B5.9 deepen).
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    /// Classify why resolve would skip before history update (B5.9 deepen).
    /// Classify why pass resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
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
