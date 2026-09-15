#include <fuse/renderer/shadow/directional_shadow.hpp>

#include <cmath>

namespace fuse::renderer {
namespace {

fuse::math::Vec3 cross(const fuse::math::Vec3& a, const fuse::math::Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

ShadowMat4 makeLookAt(const fuse::math::Vec3& eye, const fuse::math::Vec3& target, const fuse::math::Vec3& up) {
    const fuse::math::Vec3 forward = (target - eye).normalized();
    const fuse::math::Vec3 side = cross(forward, up).normalized();
    const fuse::math::Vec3 correctedUp = cross(side, forward).normalized();

    ShadowMat4 view = ShadowMat4::identity();
    view.data[0] = side.x;
    view.data[1] = correctedUp.x;
    view.data[2] = -forward.x;
    view.data[4] = side.y;
    view.data[5] = correctedUp.y;
    view.data[6] = -forward.y;
    view.data[8] = side.z;
    view.data[9] = correctedUp.z;
    view.data[10] = -forward.z;
    view.data[12] = -side.dot(eye);
    view.data[13] = -correctedUp.dot(eye);
    view.data[14] = forward.dot(eye);
    return view;
}

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
    const f32 splitFraction = m_desc.csm.cascadeSplits[cascade];
    const f32 cascadeNear =
        cascade == 0u ? camera.nearPlane
                      : CascadedShadowMapLayout::computeCascadeFarZ(cascade - 1u, m_desc.csm, camera);
    const f32 cascadeFar = CascadedShadowMapLayout::computeCascadeFarZ(cascade, m_desc.csm, camera);
    const f32 midDistance = (cascadeNear + cascadeFar) * 0.5f;

    const fuse::math::Vec3 forward = camera.forward.normalized();
    const fuse::math::Vec3 focus = camera.position + forward * midDistance;
    const fuse::math::Vec3 lightPos = focus - sunDirection * (splitFraction * camera.farPlane);

    const fuse::math::Vec3 up = {0.f, 1.f, 0.f};
    const ShadowMat4 view = makeLookAt(lightPos, focus, up);

    const f32 orthoExtent = cascadeFar * 0.5f;
    ShadowMat4 projection =
        makeOrthographic(-orthoExtent, orthoExtent, -orthoExtent, orthoExtent, cascadeNear, cascadeFar);

    if (m_desc.csm.stabilise) {
        const f32 texelWorldSize = (2.f * orthoExtent) / static_cast<f32>(m_desc.csm.resolution);
        if (texelWorldSize > 1e-8f) {
            projection.data[12] =
                std::floor(projection.data[12] / texelWorldSize + 0.5f) * texelWorldSize;
            projection.data[13] =
                std::floor(projection.data[13] / texelWorldSize + 0.5f) * texelWorldSize;
        }
    }

    m_data.lightViewProj[cascade] = multiply(projection, view);
}

} // namespace fuse::renderer
