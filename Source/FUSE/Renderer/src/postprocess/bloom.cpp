#include <fuse/renderer/postprocess/bloom.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

void Bloom::init() {
    m_ready = true;
}

void Bloom::destroy() {
    m_ready = false;
}

f32 Bloom::luminance(const fuse::math::Vec3& rgb) {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

fuse::math::Vec3 Bloom::extractBright(const fuse::math::Vec3& hdr, const BloomParams& params) {
    const f32 lum = luminance(hdr);
    if (lum <= params.threshold - params.knee) {
        return {};
    }

    f32 soft = lum - params.threshold + params.knee;
    if (lum < params.threshold + params.knee) {
        soft = (soft * soft) / (4.f * params.knee + 1e-8f);
    }
    const f32 weight = soft / (lum + 1e-8f);
    return hdr * weight;
}

fuse::math::Vec3 Bloom::apply(const fuse::math::Vec3& hdr, const BloomParams& params) {
    const fuse::math::Vec3 bright = extractBright(hdr, params);
    return hdr + bright * params.intensity;
}

void Bloom::downsampleBox(const fuse::math::Vec3* src, u32 width, u32 height, fuse::math::Vec3* dst) {
    if (src == nullptr || dst == nullptr || width < 2u || height < 2u) {
        return;
    }

    const u32 dstWidth = width / 2u;
    const u32 dstHeight = height / 2u;
    for (u32 y = 0; y < dstHeight; ++y) {
        for (u32 x = 0; x < dstWidth; ++x) {
            const u32 srcX = x * 2u;
            const u32 srcY = y * 2u;
            const fuse::math::Vec3 a = src[srcY * width + srcX];
            const fuse::math::Vec3 b = src[srcY * width + srcX + 1u];
            const fuse::math::Vec3 c = src[(srcY + 1u) * width + srcX];
            const fuse::math::Vec3 d = src[(srcY + 1u) * width + srcX + 1u];
            dst[y * dstWidth + x] = (a + b + c + d) * 0.25f;
        }
    }
}

} // namespace fuse::renderer
