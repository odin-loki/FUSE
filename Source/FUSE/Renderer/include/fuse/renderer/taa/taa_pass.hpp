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
    /// True when pass jitter monotonic counter and slot match `frameIndex` (B5.9 deepen).
    bool jitterAlignedToFrameIndex(u32 frameIndex) const;
    void invalidateHistory();
    void resize(u32 width, u32 height);
    bool matchesDimensions(u32 width, u32 height) const;
    bool needsHistoryWarmup() const { return m_history.needsWarmup(); }
    /// Frames remaining before pass history may be temporally reused (B5.9 deepen).
    u32 warmupFramesRemaining() const;
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
                                       TaaResolveBlendRejectReason& reason) const;
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
    /// History reuse preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& reason) const;
    /// History resolve-readiness preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightHistoryReadyForResolve(TaaHistoryReuseBlockReason& reason) const;
    /// Classify why resolve blend weights would be rejected (B5.9 deepen).
    TaaResolveBlendRejectReason classifyResolveBlendReject(const TaaResolveDesc& desc) const;
    /// Resolve blend preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightResolveBlendWeights(const TaaResolveDesc& desc, TaaResolveBlendRejectReason& reason) const;
    /// Compute resolve blend weights with reject-reason diagnostics (B5.9 deepen).
    bool tryComputeResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,
                                       TaaResolveBlendRejectReason& reason) const;
    /// Classify why jitter sync to a frame counter would be rejected (B5.9 deepen).
    TaaJitterGuardRejectReason classifyJitterSyncReject() const;
    /// Jitter sync preflight with mandatory reject-reason output (B5.9 deepen).
    bool tryPreflightJitterSync(u32 frameIndex, TaaJitterGuardRejectReason& reason) const;
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
    u32 historyInvalidateGeneration() const { return m_history.invalidateGeneration(); }
    /// True when a consumer's observed generation differs from pass history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    /// Preflight resolve without mutating history (delegates to `TaaResolve::wouldSkip`).
    bool wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
    /// Classify why resolve would skip — same ordering as `wouldSkipResolve` (B5.9 deepen).
    TaaResolveSkipReason classifyResolveSkip(const TaaResolveDesc& desc) const;
    /// Resolve preflight with mandatory skip-reason output (B5.9 deepen).
    bool tryPreflightResolve(const TaaResolveDesc& desc, TaaResolveSkipReason& reason) const;
    /// Stamp `observed_history_generation` from pass history when still at the no-guard sentinel.
    void stampObservedHistoryGeneration(TaaResolveDesc& desc) const;
    /// Clamp params and stamp observed generation from pass history.
    void sanitizeResolveDesc(TaaResolveDesc& desc) const;
    /// True when `observedGeneration` matches the current history invalidate epoch.
    bool isObservedHistoryGenerationCurrent(u32 observedGeneration) const;

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

// --- deepen additive from deepen-b59-taa-guards-8293 ---
    TaaHistoryWarmupPreflight preflightHistoryWarmup() const;
    TaaHistoryReusePreflight preflightHistoryReuse(u32 observedGeneration) const;
    TaaHistoryReusePreflight preflightHistoryReuseForDesc(const TaaResolveDesc& desc) const;
    TaaJitterSyncPreflight preflightJitterSync(u32 frameIndex) const;
    TaaResolveBlendPreflight preflightResolveBlend(const TaaResolveDesc& desc) const;

// --- deepen additive from deepen-b59-taa-jitter-history-preflights-ddf1 ---
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* out = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-94f6 ---
    TaaHistoryReuseRejectReason classifyHistoryReuseReject(u32 observedGeneration) const;
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaResolveBlendPreflight* out = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-2b1e ---
    bool preflightResolveBlend(const TaaResolveDesc& desc, TaaBlendWeights* weights = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-5b8c ---
                                TaaHistoryReuseRejectReason* reason = nullptr) const;
                                TaaBlendPreflightRejectReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-a831 ---
    bool preflightHistoryReuse(u32 observedGeneration) const;
                               TaaResolveBlendPreflightRejectReason* reason = nullptr) const;

// --- deepen additive from deepen-taa-b59-guards-f7b5 ---
    bool preflightResolveBlend(const TaaResolveDesc& desc) const;

// --- deepen additive from deepen-b59-taa-guards-27d7 ---
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;
    bool preflightResolveWithBlend(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                                   TaaResolveBlendRejectReason* blendReason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-117f ---
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    bool preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                               TaaResolveBlendRejectReason* blendRejectReason = nullptr) const;

// --- deepen additive from deepen-fuse-b59-taa-cd32 ---
    TaaJitterSyncRejectReason classifyJitterSyncReject(u32 frameIndex) const;
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr) const;
    bool canPreflightHistoryReuse(u32 observedGeneration) const;
    bool canPreflightHistoryWarmup() const;
    bool canPreflightResolveBlendWeights(const TaaResolveDesc& desc) const;
    bool tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& out,
    bool preflightResolveTemporalBlend(const TaaResolveDesc& desc,
                                       TaaResolveTemporalRejectReason* reason = nullptr) const;
    bool canPreflightResolveTemporalBlend(const TaaResolveDesc& desc) const;

// --- deepen additive from deepen-b59-taa-guards-1d2e ---
    bool preflightHistoryReuseForDesc(const TaaResolveDesc& desc,
    bool preflightResolveGuards(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,

// --- deepen additive from deepen-b59-taa-guards-3c58 ---
    bool preflightHistoryWarmup(TaaHistoryWarmupPhase* phase = nullptr) const;
                                       TaaResolveBlendRejectReason* blendReason = nullptr,

// --- deepen additive from deepen-b59-taa-guards-d966 ---
    bool preflightHistoryWarmup(TaaHistoryWarmupState* state = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-53dc ---
    bool preflightJitterAlignment(u32 expectedFrameIndex) const;
    bool preflightResolveDesc(const TaaResolveDesc& desc, TaaResolveDescPreflight* result = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-efe8 ---
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncBlockReason& outReason);
    bool preflightResolveTemporalAccumulation(const TaaResolveDesc& desc,
                                              TaaResolveTemporalPreflight* result = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-8394 ---
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason* reason = nullptr);
    bool tryCanBeginTemporalReuse(u32 observedGeneration, TaaHistoryReuseBlockReason* reason = nullptr) const;
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveFramePreflight* out = nullptr) const;
    bool tryExpectedResolveBlendWeights(const TaaResolveDesc& desc, TaaBlendWeights& outWeights,

// --- deepen additive from deepen-b59-taa-guards-9737 ---
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterSyncRejectReason& outReason);
    bool tryPreflightHistoryReuseForResolve(const TaaResolveDesc& desc,

// --- deepen additive from deepen-taa-b59-guards-fd0d ---
    bool preflightResolveHistoryReuse(const TaaResolveDesc& desc,

// --- deepen additive from deepen-b59-taa-guards-0400 ---
    bool preflightResolveTemporal(const TaaResolveDesc& desc,

// --- deepen additive from deepen-b59-taa-guards-614c ---
    bool wouldSkipHistoryReuse(u32 observedGeneration) const;
    bool tryPreflightHistoryReuse(u32 observedGeneration, TaaHistoryReuseBlockReason& outReason) const;
    bool wouldRejectResolveBlendWeights(const TaaResolveDesc& desc) const;
                                         TaaResolveBlendRejectReason& outReason) const;
    bool wouldSkipJitterSync(u32 frameIndex) const;
    bool trySyncJitterToFrameIndex(u32 frameIndex, TaaJitterSyncRejectReason& outReason);

// --- deepen additive from deepen-b59-taa-guards-ceb9 ---
    bool preflightHistoryWarmup(TaaHistoryWarmupRejectReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-3780 ---
    bool tryPreflightHistoryWarmup(TaaHistoryWarmupBlockReason& reason) const;

// --- deepen additive from deepen-b59-taa-guards-bd40 ---
    bool wouldSkipResolveBlend(const TaaResolveDesc& desc) const;

// --- deepen additive from deepen-b59-taa-guards-6ba7 ---
    bool preflightHistoryWarmup(TaaHistoryReuseBlockReason* reason = nullptr) const;

// --- deepen additive from deepen-b59-taa-guards-e107 ---
    bool tryCurrentJitterNdcIfReady(fuse::math::Vec2& out, TaaJitterGuardRejectReason& reason) const;
    bool trySyncJitterToFrameIndexIfReady(u32 frameIndex, TaaJitterGuardRejectReason& reason);

// --- deepen additive from deepen-taa-b59-guards-ea2b ---
    bool preflightTemporalBlend(const TaaResolveDesc& desc,

// --- deepen additive from deepen-b59-taa-guards-eb8c ---
    bool preflightResolveReuseAndBlend(const TaaResolveDesc& desc, u32 observedGeneration,
                                       TaaResolveReuseBlendRejectReason* reason = nullptr) const;
