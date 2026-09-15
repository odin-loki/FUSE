#pragma once

#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Atlas packing configuration — cascades are tiled in a 2×2 grid by default.
struct ShadowAtlasDesc {
    u32 cascadeResolution = 2048;
    u32 cascadeCount = kCascadeCount;
    u32 paddingTexels = 1;
};

struct ShadowAtlasViewport {
    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;
};

struct ShadowAtlasLayout {
    u32 atlasWidth = 0;
    u32 atlasHeight = 0;
    ShadowAtlasViewport cascadeViewports[kCascadeCount]{};
};

/// Single depth atlas backing all cascades (B5.5 stub — allocation only).
class ShadowAtlas {
public:
    ShadowAtlas() = default;

    bool init(ResourceManager& resources, const ShadowAtlasDesc& desc);
    void destroy();

    bool isReady() const { return m_ready; }
    const ShadowAtlasDesc& desc() const { return m_desc; }
    const ShadowAtlasLayout& layout() const { return m_layout; }
    TextureHandle texture() const { return m_texture; }

    static ShadowAtlasLayout computeLayout(const ShadowAtlasDesc& desc);

private:
    ResourceManager* m_resources = nullptr;
    ShadowAtlasDesc m_desc{};
    ShadowAtlasLayout m_layout{};
    TextureHandle m_texture{};
    bool m_ready = false;
};

} // namespace fuse::renderer
