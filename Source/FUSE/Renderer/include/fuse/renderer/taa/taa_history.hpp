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
    bool hasValidHistory() const { return m_validity.hasValidHistory; }
    /// True when ping-pong targets are allocated and history is warm enough to sample.
    bool canReadForResolve() const { return m_ready && m_validity.hasValidHistory; }
    /// True until the first successful resolve warms the ping-pong targets.
    bool needsWarmup() const { return !m_validity.hasValidHistory; }
    u32 accumulatedFrames() const { return m_validity.accumulatedFrames; }
    u32 invalidateGeneration() const { return m_validity.invalidateGeneration; }
    /// True when a consumer's observed generation differs from the current history epoch.
    bool isHistoryStale(u32 observedGeneration) const;
    bool matchesDimensions(u32 width, u32 height) const;
    const TaaHistoryBufferDesc& desc() const { return m_desc; }
    const TaaHistoryValidity& validity() const { return m_validity; }

    TextureHandle read() const;
    TextureHandle write() const;
    u32 activeIndex() const { return m_activeIndex; }

    void swap();
    void invalidateHistory();
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
bool historyAwaitingWarmup(const TaaHistoryBuffer& history);

} // namespace fuse::renderer
