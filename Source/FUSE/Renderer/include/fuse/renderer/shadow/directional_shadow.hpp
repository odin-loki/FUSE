#pragma once

#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/shadow/csm.hpp>
#include <fuse/renderer/shadow/shadow_atlas.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

struct DirectionalShadowDesc {
    CascadedShadowMapDesc csm{};
    ShadowAtlasDesc atlas{};
};

struct DirectionalShadowStats {
    bool ready = false;
    u32 framesUpdated = 0;
};

/// Directional sun shadow scaffold — owns cascade data and atlas allocation (B5.5).
class DirectionalShadow {
public:
    DirectionalShadow() = default;

    bool init(ResourceManager& resources, const DirectionalShadowDesc& desc = {});
    void destroy();

    void update(const ShadowCameraParams& camera, const fuse::math::Vec3& sunDirection);

    bool isReady() const { return m_stats.ready; }
    const DirectionalShadowDesc& desc() const { return m_desc; }
    const CascadedShadowMapData& data() const { return m_data; }
    const ShadowAtlas& atlas() const { return m_atlas; }
    const DirectionalShadowStats& stats() const { return m_stats; }

private:
    void computeCascadeMatrix_(u32 cascade,
                               const ShadowCameraParams& camera,
                               const fuse::math::Vec3& sunDirection);

    ResourceManager* m_resources = nullptr;
    DirectionalShadowDesc m_desc{};
    CascadedShadowMapData m_data{};
    ShadowAtlas m_atlas{};
    DirectionalShadowStats m_stats{};
};

} // namespace fuse::renderer
