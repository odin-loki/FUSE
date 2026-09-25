#include <fuse/renderer/deferred/gbuffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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

u32 GBufferLayout::channelCount(GBufferAttachment attachment) {
    switch (attachment) {
    case GBufferAttachment::NormalAo:
    case GBufferAttachment::AlbedoAlpha:
    case GBufferAttachment::RoughMetalEmissiveShading:
    case GBufferAttachment::Emissive:
        return 4u;
    case GBufferAttachment::Velocity:
        return 2u;
    case GBufferAttachment::Depth:
        return 1u;
    default:
        return 0u;
    }
}

bool GBufferLayout::validateAttachmentFormats() {
    for (u32 i = 0; i < attachmentCount(); ++i) {
        const auto attachment = static_cast<GBufferAttachment>(i);
        if (GBufferLayout::format(attachment) == GpuFormat::Undefined) {
            return false;
        }
        if (GBufferLayout::channelCount(attachment) == 0u) {
            return false;
        }
    }
    return true;
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

namespace {

f32 signNotZero(f32 v) {
    return v >= 0.f ? 1.f : -1.f;
}

} // namespace

fuse::math::Vec2 GBufferEncoding::encodeOctSigned(const fuse::math::Vec3& normal) {
    const f32 sum = std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z);
    if (sum < 1e-20f) {
        return {0.f, 0.f}; // degenerate input -> +Z
    }
    const fuse::math::Vec3 n = normal * (1.f / sum);
    if (n.z >= 0.f) {
        return {n.x, n.y};
    }
    // Lower hemisphere folds over the diagonals. sign(0) must be +1 here: GLSL sign() returns 0,
    // which collapses e.g. (0,0,-1) onto +Z.
    return {(1.f - std::fabs(n.y)) * signNotZero(n.x), (1.f - std::fabs(n.x)) * signNotZero(n.y)};
}

fuse::math::Vec3 GBufferEncoding::decodeOctSigned(const fuse::math::Vec2& oct) {
    fuse::math::Vec3 n = {oct.x, oct.y, 1.f - std::fabs(oct.x) - std::fabs(oct.y)};
    if (n.z < 0.f) {
        // Both components come from the *unfolded* values (GLSL assigns n.xy simultaneously).
        const f32 x = n.x;
        const f32 y = n.y;
        n.x = (1.f - std::fabs(y)) * signNotZero(x);
        n.y = (1.f - std::fabs(x)) * signNotZero(y);
    }
    return n.normalized();
}

fuse::math::Vec2 GBufferEncoding::encodeNormal(const fuse::math::Vec3& normal) {
    const fuse::math::Vec2 o = encodeOctSigned(normal);
    return {o.x * 0.5f + 0.5f, o.y * 0.5f + 0.5f};
}

fuse::math::Vec3 GBufferEncoding::decodeNormal(const fuse::math::Vec2& encoded) {
    return decodeOctSigned({encoded.x * 2.f - 1.f, encoded.y * 2.f - 1.f});
}

fuse::math::Vec2 GBufferEncoding::encodeNormalRgba16f(const fuse::math::Vec3& normal) {
    const fuse::math::Vec3 n = normal.normalized();
    const fuse::math::Vec2 o = encodeOctSigned(n);
    const u16 hx = GBufferQuantize::floatToHalf(o.x);
    const u16 hy = GBufferQuantize::floatToHalf(o.y);
    const u16 candX[3] = {hx, GBufferQuantize::halfNextDown(hx), GBufferQuantize::halfNextUp(hx)};
    const u16 candY[3] = {hy, GBufferQuantize::halfNextDown(hy), GBufferQuantize::halfNextUp(hy)};

    fuse::math::Vec2 best = {GBufferQuantize::halfToFloat(hx), GBufferQuantize::halfToFloat(hy)};
    f32 bestDot = -2.f;
    for (const u16 cx : candX) {
        const f32 x = GBufferQuantize::halfToFloat(cx);
        if (std::fabs(x) > 1.f) {
            continue;
        }
        for (const u16 cy : candY) {
            const f32 y = GBufferQuantize::halfToFloat(cy);
            if (std::fabs(y) > 1.f) {
                continue;
            }
            const f32 d = decodeOctSigned({x, y}).dot(n);
            if (d > bestDot) {
                bestDot = d;
                best = {x, y};
            }
        }
    }
    return best;
}

f32 GBufferEncoding::angularErrorRadians(const fuse::math::Vec3& a, const fuse::math::Vec3& b) {
    // atan2(|a x b|, a.b) stays accurate for tiny angles where acos(dot) loses all precision.
    const fuse::math::Vec3 na = a.normalized();
    const fuse::math::Vec3 nb = b.normalized();
    return std::atan2(fuse::math::cross(na, nb).length(), na.dot(nb));
}

u16 GBufferQuantize::floatToHalf(f32 value) {
    u32 x = 0;
    std::memcpy(&x, &value, sizeof(x));
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 absBits = x & 0x7FFFFFFFu;

    if (absBits >= 0x7F800000u) { // inf / NaN
        return static_cast<u16>(sign | 0x7C00u | (absBits > 0x7F800000u ? 0x0200u : 0u));
    }
    if (absBits >= 0x477FF000u) { // >= 65520 rounds past the largest finite half
        return static_cast<u16>(sign | 0x7C00u);
    }
    if (absBits < 0x38800000u) { // below 2^-14: half subnormal, value * 2^24 rounded to nearest even
        f32 absValue = 0.f;
        std::memcpy(&absValue, &absBits, sizeof(absValue));
        const u32 mantissa = static_cast<u32>(std::nearbyint(absValue * 16777216.f));
        return static_cast<u16>(sign | mantissa);
    }

    u32 h = (((absBits >> 23) - 112u) << 10) | ((absBits & 0x7FFFFFu) >> 13);
    const u32 rem = absBits & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) != 0u)) {
        ++h; // mantissa carry into the exponent is the correct result
    }
    return static_cast<u16>(sign | h);
}

f32 GBufferQuantize::halfToFloat(u16 bits) {
    const u32 sign = (static_cast<u32>(bits) & 0x8000u) << 16;
    const u32 exponent = (bits >> 10) & 0x1Fu;
    const u32 mantissa = bits & 0x3FFu;
    if (exponent == 0u) {
        const f32 v = std::ldexp(static_cast<f32>(mantissa), -24);
        return sign != 0u ? -v : v;
    }
    u32 out = 0;
    if (exponent == 31u) {
        out = sign | 0x7F800000u | (mantissa << 13);
    } else {
        out = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    }
    f32 result = 0.f;
    std::memcpy(&result, &out, sizeof(result));
    return result;
}

u16 GBufferQuantize::halfNextUp(u16 bits) {
    if ((bits & 0x7FFFu) == 0u) {
        return 0x0001u;
    }
    return (bits & 0x8000u) != 0u ? static_cast<u16>(bits - 1u) : static_cast<u16>(bits + 1u);
}

u16 GBufferQuantize::halfNextDown(u16 bits) {
    if ((bits & 0x7FFFu) == 0u) {
        return 0x8001u;
    }
    return (bits & 0x8000u) != 0u ? static_cast<u16>(bits + 1u) : static_cast<u16>(bits - 1u);
}

f32 GBufferQuantize::toUnorm8(f32 value) {
    const f32 clamped = std::clamp(value, 0.f, 1.f);
    return static_cast<f32>(std::lround(clamped * 255.f)) / 255.f;
}

GBufferMrt GBufferPacking::pack(const GBufferPackedData& data, f32 alpha) {
    GBufferMrt mrt{};
    const fuse::math::Vec2 encodedNormal = GBufferEncoding::encodeNormalRgba16f(data.normal);
    mrt.rt0 = {encodedNormal.x, encodedNormal.y, 0.f, data.ao};
    mrt.rt1 = {data.albedo.x, data.albedo.y, data.albedo.z, alpha};

    const f32 emissiveMag = data.emissive.length();
    mrt.rt2 = {data.roughness,
               data.metallic,
               emissiveMag > 0.001f ? 1.f : 0.f,
               static_cast<f32>(data.shadingModel) / 255.f};
    mrt.rt3 = {data.velocityX, data.velocityY, 0.f, 0.f};
    mrt.rt5 = {std::min(data.emissive.x, kGBufferMaxHalf),
               std::min(data.emissive.y, kGBufferMaxHalf),
               std::min(data.emissive.z, kGBufferMaxHalf),
               0.f};
    return mrt;
}

GBufferPackedData GBufferPacking::unpack(const GBufferMrt& mrt) {
    GBufferPackedData data{};
    data.normal = GBufferEncoding::decodeOctSigned({mrt.rt0.x, mrt.rt0.y});
    data.ao = mrt.rt0.w;
    data.albedo = {mrt.rt1.x, mrt.rt1.y, mrt.rt1.z};
    data.roughness = mrt.rt2.x;
    data.metallic = mrt.rt2.y;
    data.emissive = {mrt.rt5.x, mrt.rt5.y, mrt.rt5.z};
    data.velocityX = mrt.rt3.x;
    data.velocityY = mrt.rt3.y;
    data.shadingModel = static_cast<u8>(std::lround(mrt.rt2.w * 255.f));
    return data;
}

GBufferMrt GBufferPacking::quantizeToStorage(const GBufferMrt& mrt) {
    const auto h = [](const fuse::math::Vec4& v) {
        return fuse::math::Vec4{GBufferQuantize::toHalf(v.x), GBufferQuantize::toHalf(v.y),
                                GBufferQuantize::toHalf(v.z), GBufferQuantize::toHalf(v.w)};
    };
    const auto u8n = [](const fuse::math::Vec4& v) {
        return fuse::math::Vec4{GBufferQuantize::toUnorm8(v.x), GBufferQuantize::toUnorm8(v.y),
                                GBufferQuantize::toUnorm8(v.z), GBufferQuantize::toUnorm8(v.w)};
    };
    GBufferMrt out{};
    out.rt0 = h(mrt.rt0);   // RGBA16F
    out.rt1 = u8n(mrt.rt1); // RGBA8
    out.rt2 = u8n(mrt.rt2); // RGBA8
    out.rt3 = {GBufferQuantize::toHalf(mrt.rt3.x), GBufferQuantize::toHalf(mrt.rt3.y), 0.f, 0.f}; // RG16F
    out.rt5 = h(mrt.rt5);   // RGBA16F
    return out;
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

        // RT4 depth is an R32F *color* target (linear/reversed-Z depth for CUDA passes, B5.2);
        // the hardware depth buffer is separate. R32F cannot carry DEPTH_STENCIL_ATTACHMENT usage.
        // TransferSrc: attachments are read back for debugging / capture (B5 G-buffer gate).
        textureDesc.usage = static_cast<ImageUsage>(
            static_cast<u32>(ImageUsage::ColorAttachment) | static_cast<u32>(ImageUsage::Sampled) |
            static_cast<u32>(ImageUsage::Storage) | static_cast<u32>(ImageUsage::TransferSrc));

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
