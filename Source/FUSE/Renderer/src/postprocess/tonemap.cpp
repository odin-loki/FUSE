#include <fuse/renderer/postprocess/tonemap.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

fuse::math::Vec3 clamp3(const fuse::math::Vec3& value, f32 minValue, f32 maxValue) {
    return {std::clamp(value.x, minValue, maxValue), std::clamp(value.y, minValue, maxValue),
            std::clamp(value.z, minValue, maxValue)};
}

} // namespace

fuse::math::Vec3 aces_tonemap(const fuse::math::Vec3& x) {
    constexpr f32 a = 2.51f;
    constexpr f32 b = 0.03f;
    constexpr f32 c = 2.43f;
    constexpr f32 d = 0.59f;
    constexpr f32 e = 0.14f;
    const auto mapChannel = [](f32 channel) {
        return std::clamp((channel * (a * channel + b)) / (channel * (c * channel + d) + e), 0.f, 1.f);
    };
    return {mapChannel(x.x), mapChannel(x.y), mapChannel(x.z)};
}

fuse::math::Vec3 filmic_tonemap(const fuse::math::Vec3& x) {
    const auto curve = [](f32 v) {
        const f32 A = 0.22f;
        const f32 B = 0.30f;
        const f32 C = 0.10f;
        const f32 D = 0.20f;
        const f32 E = 0.01f;
        const f32 F = 0.30f;
        return ((v * (A * v + C * B) + D * E) / (v * (A * v + B) + D * F)) - E / F;
    };
    return clamp3({curve(x.x), curve(x.y), curve(x.z)}, 0.f, 1.f);
}

fuse::math::Vec3 reinhard_tonemap(const fuse::math::Vec3& x) {
    return clamp3({x.x / (1.f + x.x), x.y / (1.f + x.y), x.z / (1.f + x.z)}, 0.f, 1.f);
}

fuse::math::Vec3 apply_tone_map(const fuse::math::Vec3& hdr, ToneMapper mapper) {
    switch (mapper) {
    case ToneMapper::ACES:
        return aces_tonemap(hdr);
    case ToneMapper::Filmic:
        return filmic_tonemap(hdr);
    case ToneMapper::Reinhard:
        return reinhard_tonemap(hdr);
    case ToneMapper::Neutral:
        return clamp3(hdr, 0.f, 1.f);
    default:
        return aces_tonemap(hdr);
    }
}

const char* tone_mapper_name(ToneMapper mapper) {
    switch (mapper) {
    case ToneMapper::ACES:
        return "aces";
    case ToneMapper::Filmic:
        return "filmic";
    case ToneMapper::Reinhard:
        return "reinhard";
    case ToneMapper::Neutral:
        return "neutral";
    default:
        return "unknown";
    }
}

f32 tone_mapper_mid_grey_calibration_ev(ToneMapper mapper, f32 scene_grey, f32 display_grey) {
    if (scene_grey <= 0.f || display_grey <= 0.f) {
        return 0.f;
    }
    const auto evaluate = [mapper, scene_grey](f32 ev) {
        const f32 x = scene_grey * std::exp2(ev);
        return apply_tone_map({x, x, x}, mapper).x;
    };
    f32 lo = -16.f;
    f32 hi = 16.f;
    if (evaluate(lo) > display_grey || evaluate(hi) < display_grey) {
        return 0.f;
    }
    for (u32 i = 0; i < 64u; ++i) {
        const f32 mid = 0.5f * (lo + hi);
        if (evaluate(mid) < display_grey) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    const f32 ev = 0.5f * (lo + hi);
    return std::fabs(ev) < 1e-6f ? 0.f : ev;
}

void ToneMap::init() {
    m_ready = true;
}

void ToneMap::destroy() {
    m_ready = false;
}

fuse::math::Vec3 ToneMap::apply(const fuse::math::Vec3& hdr) const {
    return apply_tone_map(hdr, m_mapper);
}

} // namespace fuse::renderer
