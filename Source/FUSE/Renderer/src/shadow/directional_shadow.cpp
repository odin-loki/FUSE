#include <fuse/renderer/shadow/directional_shadow.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

ShadowMat4 makeOrthographic(f32 left, f32 right, f32 bottom, f32 top, f32 nearPlane, f32 farPlane) {
    ShadowMat4 projection = ShadowMat4::identity();
    projection.data[0] = 2.f / (right - left);
    projection.data[5] = 2.f / (top - bottom);
    projection.data[10] = -1.f / (farPlane - nearPlane);
    projection.data[12] = -(right + left) / (right - left);
    projection.data[13] = -(top + bottom) / (top - bottom);
    projection.data[14] = -nearPlane / (farPlane - nearPlane);
    return projection;
}

ShadowMat4 multiply(const ShadowMat4& a, const ShadowMat4& b) {
    ShadowMat4 out{};
    for (u32 column = 0; column < 4u; ++column) {
        for (u32 row = 0; row < 4u; ++row) {
            f32 sum = 0.f;
            for (u32 k = 0; k < 4u; ++k) {
                sum += a.data[k * 4u + row] * b.data[column * 4u + k];
            }
            out.data[column * 4u + row] = sum;
        }
    }
    return out;
}

ShadowMat4 fromMat4(const fuse::math::Mat4& matrix) {
    ShadowMat4 out{};
    out.data = matrix.data;
    return out;
}

} // namespace

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

    const fuse::math::Vec3 lightDir = sunDirection.normalized();
    for (u32 cascade = 0; cascade < kCascadeCount; ++cascade) {
        computeCascadeMatrix_(cascade, camera, lightDir);
        m_data.cascadeFarZ[cascade] =
            CascadedShadowMapLayout::computeCascadeFarZ(cascade, m_desc.csm, camera);
    }

    ++m_stats.framesUpdated;
}

void DirectionalShadow::computeCascadeMatrix_(u32 cascade,
                                              const ShadowCameraParams& camera,
                                              const fuse::math::Vec3& sunDirection) {
    const CascadeRange range = CascadedShadowMapLayout::computeCascadeRange(cascade, m_desc.csm, camera);
    const f32 midDistance = (range.nearZ + range.farZ) * 0.5f;

    const fuse::math::Vec3 forward = camera.forward.normalized();
    const fuse::math::Vec3 focus = camera.position + forward * midDistance;

    const fuse::math::AABB lightAabb =
        CascadeLightSpaceLayout::computeCascadeLightSpaceAabb(cascade, m_desc.csm, camera, sunDirection);
    const ShadowMat4 view = fromMat4(CascadeLightSpaceLayout::buildLightView(focus, sunDirection));

    f32 left = lightAabb.min.x;
    f32 right = lightAabb.max.x;
    f32 bottom = lightAabb.min.y;
    f32 top = lightAabb.max.y;
    const f32 nearPlane = -lightAabb.max.z;
    const f32 farPlane = -lightAabb.min.z;

    if (m_desc.csm.stabilise) {
        const f32 resolution = static_cast<f32>(m_desc.csm.resolution);
        const f32 texelWorldSizeX = (right - left) / resolution;
        const f32 texelWorldSizeY = (top - bottom) / resolution;
        if (texelWorldSizeX > 1e-8f && texelWorldSizeY > 1e-8f) {
            const f32 centerX = (left + right) * 0.5f;
            const f32 centerY = (bottom + top) * 0.5f;
            const f32 halfExtentX =
                std::ceil((right - left) * 0.5f / texelWorldSizeX + 0.5f) * texelWorldSizeX;
            const f32 halfExtentY =
                std::ceil((top - bottom) * 0.5f / texelWorldSizeY + 0.5f) * texelWorldSizeY;
            const f32 snappedCenterX = std::floor(centerX / texelWorldSizeX + 0.5f) * texelWorldSizeX;
            const f32 snappedCenterY = std::floor(centerY / texelWorldSizeY + 0.5f) * texelWorldSizeY;
            left = snappedCenterX - halfExtentX;
            right = snappedCenterX + halfExtentX;
            bottom = snappedCenterY - halfExtentY;
            top = snappedCenterY + halfExtentY;
        }
    }

    const ShadowMat4 projection = makeOrthographic(left, right, bottom, top, nearPlane, farPlane);
    m_data.lightViewProj[cascade] = multiply(projection, view);
}

} // namespace fuse::renderer
