#include <fuse/renderer/shadow/directional_shadow.hpp>

namespace fuse::renderer {

bool DirectionalShadow::init(ResourceManager& resources, const DirectionalShadowDesc& desc) {
    destroy();

    m_resources = &resources;
    m_desc = desc;
    m_desc.atlas.cascadeCount = kCascadeCount;
    m_desc.atlas.cascadeResolution = m_desc.csm.resolution;

    if (!m_atlas.init(resources, m_desc.atlas)) {
        destroy();
        return false;
    }

    for (u32 i = 0; i < kCascadeCount; ++i) {
        TextureDesc cascadeDesc{};
        cascadeDesc.width = m_desc.csm.resolution;
        cascadeDesc.height = m_desc.csm.resolution;
        cascadeDesc.format = CascadedShadowMapLayout::depthFormat();
        cascadeDesc.usage = static_cast<ImageUsage>(
            static_cast<u32>(ImageUsage::DepthStencilAttachment) | static_cast<u32>(ImageUsage::Sampled));
        cascadeDesc.name = CascadedShadowMapLayout::debugName(i);

        m_data.shadowMaps[i] = resources.createTexture(cascadeDesc);
        if (!m_data.shadowMaps[i].isValid()) {
            destroy();
            return false;
        }
    }

    m_stats.ready = true;
    return true;
}

void DirectionalShadow::destroy() {
    if (m_resources != nullptr) {
        for (u32 i = 0; i < kCascadeCount; ++i) {
            if (m_data.shadowMaps[i].isValid()) {
                m_resources->destroyTexture(m_data.shadowMaps[i]);
                m_data.shadowMaps[i] = TextureHandle{};
            }
        }
    }

    m_atlas.destroy();
    m_resources = nullptr;
    m_desc = {};
    m_data = {};
    m_stats = {};
}

void DirectionalShadow::update(const ShadowCameraParams& camera, const fuse::math::Vec3& sunDirection) {
    if (!m_stats.ready) {
        return;
    }

    CascadeShadowDataLayout::populateCascadeShadowData(m_desc.csm, camera, sunDirection, kCascadeCount, m_data);
    ++m_stats.framesUpdated;
}

void DirectionalShadow::computeCascadeMatrix_(u32 cascade,
                                              const ShadowCameraParams& camera,
                                              const fuse::math::Vec3& sunDirection) {
    if (CascadeLightSpaceLayout::shouldSkipCascadeShadowBuild(cascade, m_desc.csm, camera, sunDirection)) {
        CascadeShadowDataLayout::clearCascadeSlot(cascade, m_data);
        return;
    }

    const CascadeLightSpaceMatrices matrices =
        CascadeLightSpaceLayout::buildCascadeLightSpaceMatrices(cascade, m_desc.csm, camera, sunDirection);
    if (matrices.valid) {
        m_data.lightViewProj[cascade] = matrices.lightViewProj;
    } else {
        CascadeShadowDataLayout::clearCascadeSlot(cascade, m_data);
    }
}

} // namespace fuse::renderer
