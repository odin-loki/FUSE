#include <fuse/renderer/deferred/gbuffer.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

GpuFormat GBufferLayout::format(GBufferAttachment attachment) {
    switch (attachment) {
    case GBufferAttachment::NormalAo:
        return GpuFormat::R16G16B16A16Sfloat;
    case GBufferAttachment::AlbedoAlpha:
    case GBufferAttachment::RoughMetalEmissiveShading:
        return GpuFormat::R8G8B8A8Unorm;
    case GBufferAttachment::Velocity:
        return GpuFormat::R16G16Sfloat;
    case GBufferAttachment::Depth:
        return GpuFormat::R32Sfloat;
    case GBufferAttachment::Emissive:
        return GpuFormat::R16G16B16A16Sfloat;
    default:
        return GpuFormat::Undefined;
    }
}

const char* GBufferLayout::debugName(GBufferAttachment attachment) {
    switch (attachment) {
    case GBufferAttachment::NormalAo:
        return "gbuffer_normal_ao";
    case GBufferAttachment::AlbedoAlpha:
        return "gbuffer_albedo_alpha";
    case GBufferAttachment::RoughMetalEmissiveShading:
        return "gbuffer_rough_metal_emissive_shading";
    case GBufferAttachment::Velocity:
        return "gbuffer_velocity";
    case GBufferAttachment::Depth:
        return "gbuffer_depth";
    case GBufferAttachment::Emissive:
        return "gbuffer_emissive";
    default:
        return "gbuffer_unknown";
    }
}

fuse::math::Vec2 GBufferEncoding::encodeNormal(const fuse::math::Vec3& normal) {
    fuse::math::Vec3 n = normal.normalized();
    const f32 sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    if (sum > 1e-8f) {
        n = n * (1.f / sum);
    }

    fuse::math::Vec2 o{};
    if (n.z >= 0.f) {
        o.x = n.x;
        o.y = n.y;
    } else {
        o.x = (1.f - std::fabs(n.y)) * (n.x >= 0.f ? 1.f : -1.f);
        o.y = (1.f - std::fabs(n.x)) * (n.y >= 0.f ? 1.f : -1.f);
    }

    return {o.x * 0.5f + 0.5f, o.y * 0.5f + 0.5f};
}

fuse::math::Vec3 GBufferEncoding::decodeNormal(const fuse::math::Vec2& encoded) {
    fuse::math::Vec2 enc = {encoded.x * 2.f - 1.f, encoded.y * 2.f - 1.f};
    fuse::math::Vec3 n = {enc.x, enc.y, 1.f - std::fabs(enc.x) - std::fabs(enc.y)};
    if (n.z < 0.f) {
        const f32 signX = n.x >= 0.f ? 1.f : -1.f;
        const f32 signY = n.y >= 0.f ? 1.f : -1.f;
        n.x = (1.f - std::fabs(n.y)) * signX;
        n.y = (1.f - std::fabs(n.x)) * signY;
    }
    return n.normalized();
}

f32 GBufferEncoding::angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b) {
    const f32 dot = std::clamp(a.normalized().dot(b.normalized()), -1.f, 1.f);
    return std::acos(dot);
}

bool GBuffer::init(ResourceManager& resources, const GBufferDesc& desc) {
    releaseTargets();
    m_resources = &resources;
    m_desc = desc;
    m_ready = false;
    if (m_desc.width == 0u || m_desc.height == 0u) {
        return false;
    }

    for (u32 i = 0; i < GBufferLayout::attachmentCount(); ++i) {
        const auto attachment = static_cast<GBufferAttachment>(i);
        TextureDesc textureDesc{};
        textureDesc.width = m_desc.width;
        textureDesc.height = m_desc.height;
        textureDesc.format = GBufferLayout::format(attachment);
        textureDesc.name = GBufferLayout::debugName(attachment);
        textureDesc.cudaInterop = true;

        if (attachment == GBufferAttachment::Depth) {
            textureDesc.usage = static_cast<ImageUsage>(
                static_cast<u32>(ImageUsage::DepthStencilAttachment) |
                static_cast<u32>(ImageUsage::Sampled));
        } else {
            textureDesc.usage = static_cast<ImageUsage>(
                static_cast<u32>(ImageUsage::ColorAttachment) |
                static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::Storage));
        }

        m_targets.attachments[i] = resources.createTexture(textureDesc);
        if (!m_targets.attachments[i].isValid()) {
            releaseTargets();
            return false;
        }
    }

    m_ready = true;
    return true;
}

void GBuffer::resize(u32 width, u32 height) {
    if (!m_ready || m_resources == nullptr) {
        return;
    }
    if (width == m_desc.width && height == m_desc.height) {
        return;
    }

    releaseTargets();
    m_desc.width = width;
    m_desc.height = height;
    (void)init(*m_resources, m_desc);
}

void GBuffer::destroy() {
    releaseTargets();
    m_resources = nullptr;
    m_desc = {};
    m_ready = false;
}

void GBuffer::releaseTargets() {
    if (m_resources == nullptr) {
        m_targets = {};
        return;
    }

    for (usize i = 0; i < static_cast<usize>(GBufferAttachment::Count); ++i) {
        if (m_targets.attachments[i].isValid()) {
            m_resources->destroyTexture(m_targets.attachments[i]);
            m_targets.attachments[i] = TextureHandle{};
        }
    }
}

} // namespace fuse::renderer
