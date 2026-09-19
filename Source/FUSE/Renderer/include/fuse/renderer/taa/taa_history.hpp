#pragma once

#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/resources.hpp>
#include <fuse/renderer/taa/taa_types.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Ping-pong colour history targets for temporal accumulation (B5.9 stub).
class TaaHistoryBuffer {
public:
    bool init(ResourceManager& resources, const TaaHistoryBufferDesc& desc);
    void resize(u32 width, u32 height);
    void destroy();

    bool isReady() const { return m_ready; }
    /// True when history buffers are allocated and ready for resolve (B5.9 deepen).
    bool readyForResolve() const;
    bool hasValidHistory() const { return m_validity.hasValidHistory; }
    /// True when history targets are ready and warmed for temporal reuse (B5.9 deepen).
    bool canReuseHistory() const;
    /// True when ping-pong targets are allocated and history is warm enough to sample.
    /// True when ping-pong targets are allocated and history is warm enough to sample (B5.9 deepen).
    bool canReadForResolve() const { return m_ready && m_validity.hasValidHistory; }
    bool canReuseHistory() const { return m_ready && m_validity.hasValidHistory; }
    /// True when the read target is ready, warmed, and bound (B5.9 deepen).
    bool hasReadableHistory() const;
    /// True when warmed history may be reused for the observed invalidate epoch (B5.9 deepen).
    bool temporalReuseAllowed(u32 observedGeneration) const;
    /// True when ping-pong targets are allocated and the first resolve has completed (B5.9 deepen).
    bool warmupComplete() const;
    /// True when the next resolve would be the warm-up frame (B5.9 deepen).
    bool isWarmupFrame() const;
    /// True when temporal history reuse is allowed for the observed invalidate epoch (B5.9 deepen).
    bool preflightReuse(u32 observedGeneration) const;
    /// True when history is allocated and warmed for temporal reuse (B5.9 deepen).
    /// True until the first successful resolve warms the ping-pong targets.
    bool needsWarmup() const { return !m_validity.hasValidHistory; }
    /// True when history targets are allocated and warmed for temporal reuse (B5.9 deepen).
    /// True when history is warmed and no longer needs a warm-up frame (B5.9 deepen).
    /// True when warm-up is complete and temporal reuse may proceed (B5.9 deepen).
    /// True when history is allocated and warmed for temporal reuse (B5.9 deepen).
    bool warmupComplete() const;
    /// True when history has completed warm-up (B5.9 deepen).
    bool isWarmed() const { return m_validity.hasValidHistory; }
    /// Frames remaining before temporal reuse is allowed — 0 when warmed (B5.9 deepen).
    u32 warmupFramesRemaining() const;
    /// True when history warm-up is complete (B5.9 deepen).
    /// True when history has completed warm-up and may be temporally reused (B5.9 deepen).
    bool warmupComplete() const;
    /// True when history is ready, warmed, and generation matches for reuse (B5.9 deepen).
    bool reuseReady(u32 observedGeneration) const;
    /// True when history targets are ready and warmed for temporal reuse (B5.9 deepen).
    bool warmupComplete() const;
    /// Early-out when temporal reuse should be skipped for the observed epoch (B5.9 deepen).
    bool shouldSkipReuse(u32 observedGeneration) const;
    /// True when history targets are allocated and ready for resolve (B5.9 deepen).
    bool readyForResolve() const;
    /// True when history is warmed for temporal reuse (B5.9 deepen).
    /// Current warm-up lifecycle state (B5.9 deepen).
    TaaHistoryWarmupState warmupState() const;
    u32 accumulatedFrames() const { return m_validity.accumulatedFrames; }
    u32 invalidateGeneration() const { return m_validity.invalidateGeneration; }
    /// True when a consumer's observed generation differs from the current history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    /// True when `observedGeneration` matches the current invalidate epoch.
    bool generationMatches(u32 observedGeneration) const;
    /// True when `observedGeneration` matches the current history invalidate epoch.
    /// True when history targets are allocated and match the resolve dimensions.
    bool canAcceptResolveAt(u32 width, u32 height) const;
    /// True when observed epoch matches current invalidate generation (B5.9 deepen).
    bool isGenerationCurrent(u32 observedGeneration) const;
    bool matchesDimensions(u32 width, u32 height) const;
    const TaaHistoryBufferDesc& desc() const { return m_desc; }
    const TaaHistoryValidity& validity() const { return m_validity; }

    TextureHandle read() const;
    TextureHandle write() const;
    u32 activeIndex() const { return m_activeIndex; }

    void swap();
    void invalidateHistory();
    /// Invalidate when `observedGeneration` differs from the current epoch; returns true when invalidated.
    bool invalidateHistoryIfStale(u32 observedGeneration);
    void markResolved();

private:
    void releaseTargets();

    ResourceManager* m_resources = nullptr;
    TaaHistoryBufferDesc m_desc{};
    TaaHistoryValidity m_validity{};
    TextureHandle m_buffers[2]{};
    u32 m_activeIndex = 0;
    bool m_ready = false;
};

/// True when history targets are allocated and warmed for resolve input reuse.
bool canReadHistoryForResolve(const TaaHistoryBuffer& history);
/// True when history is allocated but still awaiting the first successful resolve.
/// True when history targets are allocated and warmed for resolve input reuse (B5.9 deepen).
/// True when history is allocated but still awaiting the first successful resolve (B5.9 deepen).
bool historyAwaitingWarmup(const TaaHistoryBuffer& history);

} // namespace fuse::renderer
