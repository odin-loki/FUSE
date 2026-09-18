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

f32 hill_aces_channel(f32 channel, f32 contrast, f32 shoulder) {
    constexpr f32 a = 2.51f;
    constexpr f32 b = 0.03f;
    constexpr f32 c = 2.43f;
    constexpr f32 d = 0.59f;
    constexpr f32 e = 0.14f;
    const f32 scaled = channel * contrast;
    const f32 mapped = (scaled * (a * scaled + b)) / (scaled * (c * scaled + d) + e);
    const f32 shoulderBlend = mapped / (1.f + shoulder * mapped);
    return shoulderBlend;
}

} // namespace

TonemapCurveParams make_filmic_curve_params(const TonemapCurveParams& overrides) {
    TonemapCurveParams params{};
    params.kind = TonemapCurveKind::Filmic;
    params.enabled = true;
    params.toe_strength = overrides.toe_strength;
    params.toe_length = overrides.toe_length;
    params.shoulder_strength = overrides.shoulder_strength;
    params.shoulder_length = overrides.shoulder_length;
    params.shoulder_angle = overrides.shoulder_angle;
    params.gamma = overrides.gamma;
    return params;
}

TonemapCurveParams make_reinhard_curve_params(const ReinhardCurveParams& reinhard) {
    TonemapCurveParams params{};
    params.kind = TonemapCurveKind::Reinhard;
    params.enabled = true;
    params.reinhard = reinhard;
    return params;
}

TonemapCurveParams make_aces_curve_params(const AcesCurveParams& aces) {
    TonemapCurveParams params{};
    params.kind = TonemapCurveKind::ACES;
    params.enabled = true;
    params.aces = aces;
    return params;
}

TonemapCurveEndpoints evaluate_tonemap_curve_endpoints(const TonemapCurveParams& params, f32 white_input) {
    TonemapCurveEndpoints endpoints{};
    endpoints.black_input = 0.f;
    endpoints.white_input = std::max(white_input, 0.f);
    endpoints.black_output = evaluate_tonemap_curve_channel(0.f, params);
    endpoints.white_output = evaluate_tonemap_curve_channel(endpoints.white_input, params);
    return endpoints;
}

f32 tonemap_curve_output_span(const TonemapCurveParams& params, f32 white_input) {
    const TonemapCurveEndpoints endpoints = evaluate_tonemap_curve_endpoints(params, white_input);
    return endpoints.white_output - endpoints.black_output;
}

bool tonemap_curve_preserves_black(const TonemapCurveParams& params, f32 epsilon) {
    const f32 black = evaluate_tonemap_curve_channel(0.f, params);
    return black <= epsilon;
}

bool tonemap_curve_params_valid(const TonemapCurveParams& params) {
    if (!params.enabled) {
        return true;
    }
    if (params.gamma <= 0.f) {
        return false;
    }
    if (params.kind == TonemapCurveKind::Reinhard && params.reinhard.white_point <= 0.f) {
        return false;
    }
    if (params.kind == TonemapCurveKind::ACES && params.aces.contrast < 0.f) {
        return false;
    }
    if (params.kind == TonemapCurveKind::ACES && params.aces.shoulder < 0.f) {
        return false;
    }
    return true;
}

bool tonemap_curve_can_apply(const TonemapCurveParams& params) {
    if (!params.enabled) {
        return true;
    }
    return tonemap_curve_params_valid(params);
}

bool tonemap_curve_channel_in_display_range(f32 channel, f32 epsilon) {
    return channel >= -epsilon && channel <= 1.f + epsilon;
}

bool tonemap_curve_endpoints_valid(const TonemapCurveEndpoints& endpoints, f32 epsilon) {
    if (endpoints.black_output < 0.f || endpoints.black_output > epsilon) {
        return false;
    }
    if (endpoints.white_output < 0.f || endpoints.white_output > 1.f + epsilon) {
        return false;
    }
    return endpoints.white_output > endpoints.black_output + epsilon;
}

bool tonemap_curve_endpoint_inputs_valid(const TonemapCurveEndpoints& endpoints, f32 epsilon) {
    if (endpoints.black_input < -epsilon) {
        return false;
    }
    return endpoints.white_input > endpoints.black_input + epsilon;
}

bool tonemap_curve_white_input_valid(f32 white_input, f32 epsilon) {
    return white_input > epsilon;
}

bool tonemap_curve_has_valid_endpoints(const TonemapCurveParams& params, f32 white_input, f32 epsilon) {
    if (!params.enabled) {
        return true;
    }
    if (!tonemap_curve_params_valid(params)) {
        return false;
    }
    if (!tonemap_curve_white_input_valid(white_input, epsilon)) {
        return false;
    }
    const TonemapCurveEndpoints endpoints = evaluate_tonemap_curve_endpoints(params, white_input);
    return tonemap_curve_endpoint_inputs_valid(endpoints, epsilon) &&
           tonemap_curve_endpoints_valid(endpoints, epsilon);
}

f32 tonemap_curve_mid_grey_output(const TonemapCurveParams& params, f32 mid_grey) {
    return evaluate_tonemap_curve_channel(std::max(mid_grey, 0.f), params);
}

fuse::math::Vec3 apply_exposure_ev(const fuse::math::Vec3& hdr, f32 ev_stops) {
    const f32 scale = std::pow(2.f, ev_stops);
    return {hdr.x * scale, hdr.y * scale, hdr.z * scale};
}

f32 evaluate_reinhard_curve_channel(f32 channel, const ReinhardCurveParams& params) {
    const f32 exposed = std::max(channel, 0.f) * std::pow(2.f, params.exposure_bias);
    const f32 whitePoint = std::max(params.white_point, 1e-4f);
    const f32 numerator = exposed * (1.f + exposed / (whitePoint * whitePoint));
    const f32 denominator = 1.f + exposed;
    return std::clamp(numerator / denominator, 0.f, 1.f);
}

f32 evaluate_aces_curve_channel(f32 channel, const AcesCurveParams& params) {
    return std::clamp(hill_aces_channel(std::max(channel, 0.f), params.contrast, params.shoulder), 0.f, 1.f);
}

f32 evaluate_tonemap_curve_channel(f32 channel, const TonemapCurveParams& params) {
    if (!params.enabled) {
        return channel;
    }

    switch (params.kind) {
    case TonemapCurveKind::Reinhard:
        return evaluate_reinhard_curve_channel(channel, params.reinhard);
    case TonemapCurveKind::ACES:
        return evaluate_aces_curve_channel(channel, params.aces);
    case TonemapCurveKind::Filmic:
    default:
        return std::clamp(evaluate_filmic_curve(channel, params), 0.f, 1.f);
    }
}

fuse::math::Vec3 apply_tonemap_curve(const fuse::math::Vec3& hdr, const TonemapCurveParams& params) {
    if (!params.enabled || !tonemap_curve_can_apply(params)) {
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
