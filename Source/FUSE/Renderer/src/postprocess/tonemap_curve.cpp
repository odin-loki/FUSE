#include <fuse/renderer/postprocess/tonemap_curve.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

f32 filmic_segment(f32 x, f32 A, f32 B, f32 C, f32 D, f32 E, f32 F) {
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

f32 evaluate_filmic_curve(f32 x, const TonemapCurveParams& params) {
    const f32 A = params.shoulder_strength + 0.22f;
    const f32 B = params.toe_length + 0.30f;
    const f32 C = params.toe_strength + 0.10f;
    const f32 D = params.shoulder_length + 0.20f;
    const f32 E = 0.01f;
    const f32 F = params.shoulder_angle + 0.30f;
    const f32 whiteScale = 1.f / (filmic_segment(11.2f, A, B, C, D, E, F) + 1e-8f);
    const f32 mapped = filmic_segment(x, A, B, C, D, E, F) * whiteScale;
    if (params.gamma != 1.f) {
        return std::pow(std::max(mapped, 0.f), 1.f / params.gamma);
    }
    return mapped;
}

} // namespace

f32 evaluate_tonemap_curve_channel(f32 channel, const TonemapCurveParams& params) {
    if (!params.enabled) {
        return channel;
    }
    return std::clamp(evaluate_filmic_curve(channel, params), 0.f, 1.f);
}

fuse::math::Vec3 apply_tonemap_curve(const fuse::math::Vec3& hdr, const TonemapCurveParams& params) {
    if (!params.enabled) {
        return hdr;
    }
    return {evaluate_tonemap_curve_channel(hdr.x, params), evaluate_tonemap_curve_channel(hdr.y, params),
            evaluate_tonemap_curve_channel(hdr.z, params)};
}

void TonemapCurve::init() {
    m_ready = true;
}

void TonemapCurve::destroy() {
    m_ready = false;
}

fuse::math::Vec3 TonemapCurve::apply(const fuse::math::Vec3& hdr) const {
    return apply_tonemap_curve(hdr, m_params);
}

} // namespace fuse::renderer
