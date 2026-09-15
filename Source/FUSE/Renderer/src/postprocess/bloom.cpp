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

} // namespace fuse::renderer
