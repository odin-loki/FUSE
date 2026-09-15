#include <fuse/renderer/shadow/shadow_atlas.hpp>

namespace fuse::renderer {

ShadowAtlasLayout ShadowAtlas::computeLayout(const ShadowAtlasDesc& desc) {
    ShadowAtlasLayout layout{};

    const u32 cascadeCount = desc.cascadeCount > 0u ? desc.cascadeCount : 1u;
    const u32 columns = cascadeCount <= 2u ? cascadeCount : 2u;
    const u32 rows = (cascadeCount + columns - 1u) / columns;

    const u32 tileSize = desc.cascadeResolution + desc.paddingTexels;
    layout.atlasWidth = columns * tileSize;
    layout.atlasHeight = rows * tileSize;

    for (u32 i = 0; i < kCascadeCount; ++i) {
        if (i >= cascadeCount) {
            layout.cascadeViewports[i] = {};
            continue;
        }

        const u32 column = i % columns;
        const u32 row = i / columns;
        layout.cascadeViewports[i].x = column * tileSize;
        layout.cascadeViewports[i].y = row * tileSize;
        layout.cascadeViewports[i].width = desc.cascadeResolution;
        layout.cascadeViewports[i].height = desc.cascadeResolution;
    }

    return layout;
}

bool ShadowAtlas::init(ResourceManager& resources, const ShadowAtlasDesc& desc) {
    destroy();

    if (desc.cascadeResolution == 0u || desc.cascadeCount == 0u) {
        return false;
    }

    m_resources = &resources;
    m_desc = desc;
    m_layout = computeLayout(desc);

    TextureDesc textureDesc{};
    textureDesc.width = m_layout.atlasWidth;
    textureDesc.height = m_layout.atlasHeight;
    textureDesc.format = CascadedShadowMapLayout::depthFormat();
    textureDesc.usage = static_cast<ImageUsage>(
        static_cast<u32>(ImageUsage::DepthStencilAttachment) | static_cast<u32>(ImageUsage::Sampled));
    textureDesc.name = "shadow_atlas";
    textureDesc.cudaInterop = false;

    m_texture = resources.createTexture(textureDesc);
    if (!m_texture.isValid()) {
        destroy();
        return false;
    }

    m_ready = true;
    return true;
}

void ShadowAtlas::destroy() {
    if (m_resources != nullptr && m_texture.isValid()) {
        m_resources->destroyTexture(m_texture);
    }

    m_resources = nullptr;
    m_desc = {};
    m_layout = {};
    m_texture = TextureHandle{};
    m_ready = false;
}

} // namespace fuse::renderer
