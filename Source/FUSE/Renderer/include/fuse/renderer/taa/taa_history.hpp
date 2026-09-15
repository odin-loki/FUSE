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
    const TaaHistoryBufferDesc& desc() const { return m_desc; }

    TextureHandle read() const;
    TextureHandle write() const;
    u32 activeIndex() const { return m_activeIndex; }

    void swap();

private:
    void releaseTargets();

    ResourceManager* m_resources = nullptr;
    TaaHistoryBufferDesc m_desc{};
    TextureHandle m_buffers[2]{};
    u32 m_activeIndex = 0;
    bool m_ready = false;
};

} // namespace fuse::renderer
