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
    /// True when pass history is warmed and may be sampled (B5.9 deepen).
    bool canReuseHistory() const;
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
    /// True when pass history is warmed for temporal contribution (B5.9 deepen).
    bool preflightHistoryWarmup(TaaHistoryWarmupBlockReason* reason = nullptr) const;
    /// Frames remaining before pass history may be reused — 0 when warmed (B5.9 deepen).
    u32 warmupFramesRemaining() const;
    /// True when pass history reuse is allowed for a resolve request (B5.9 deepen).
    bool preflightHistoryReuseForResolve(const TaaResolveDesc& desc,
                                         TaaHistoryReuseBlockReason* reason = nullptr) const;
    /// True when pass jitter is aligned to `frameIndex` (B5.9 deepen).
    bool preflightJitterSync(u32 frameIndex, TaaJitterSyncBlockReason* reason = nullptr) const;
    /// True when expected resolve blend weights pass validation and reuse policy (B5.9 deepen).
    bool preflightResolveBlendWeights(const TaaResolveDesc& desc,
                                      TaaResolveBlendRejectReason* reason = nullptr) const;
    /// True when resolve skip and blend-weight preflights both pass (B5.9 deepen).
    bool preflightResolveFrame(const TaaResolveDesc& desc, TaaResolveSkipReason* skipReason = nullptr,
                               TaaResolveBlendRejectReason* blendRejectReason = nullptr) const;
    u32 historyInvalidateGeneration() const { return m_history.invalidateGeneration(); }
    /// True when a consumer's observed generation differs from pass history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    /// Preflight resolve without mutating history (delegates to `TaaResolve::wouldSkip`).
    bool wouldSkipResolve(const TaaResolveDesc& desc, TaaResolveSkipReason* reason = nullptr) const;
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
