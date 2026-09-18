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
    void invalidateHistory();
    void resize(u32 width, u32 height);
    bool matchesDimensions(u32 width, u32 height) const;
    bool needsHistoryWarmup() const { return m_history.needsWarmup(); }
    u32 historyInvalidateGeneration() const { return m_history.invalidateGeneration(); }
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
