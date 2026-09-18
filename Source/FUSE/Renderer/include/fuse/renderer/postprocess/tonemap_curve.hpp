#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Pre-tonemap curve family (B5.10 deepen).
enum class TonemapCurveKind : u8 {
    Filmic = 0,
    Reinhard = 1,
    ACES = 2,
};

/// Reinhard extended curve knobs applied before the tone-map operator.
struct ReinhardCurveParams {
    f32 white_point = 4.f;
    f32 exposure_bias = 0.f;
};

/// ACES Hill-approx curve knobs applied before the tone-map operator.
struct AcesCurveParams {
    f32 contrast = 1.f;
    f32 shoulder = 1.f;
};

/// Filmic S-curve parameters applied before the tone-map operator (B5.10 deepen).
struct TonemapCurveParams {
    TonemapCurveKind kind = TonemapCurveKind::Filmic;
    f32 toe_strength = 0.f;
    f32 toe_length = 0.5f;
    f32 shoulder_strength = 0.f;
    f32 shoulder_length = 0.5f;
    f32 shoulder_angle = 1.f;
    f32 gamma = 1.f;
    bool enabled = false;
    ReinhardCurveParams reinhard{};
    AcesCurveParams aces{};
};

TonemapCurveParams make_filmic_curve_params(const TonemapCurveParams& overrides = {});
TonemapCurveParams make_reinhard_curve_params(const ReinhardCurveParams& reinhard = {});
TonemapCurveParams make_aces_curve_params(const AcesCurveParams& aces = {});

/// Canonical black/white anchor outputs for curve calibration (B5.10 deepen).
struct TonemapCurveEndpoints {
    f32 black_input = 0.f;
    f32 white_input = 1.f;
    f32 black_output = 0.f;
    f32 white_output = 0.f;
};

TonemapCurveEndpoints evaluate_tonemap_curve_endpoints(const TonemapCurveParams& params,
                                                         f32 white_input = 1.f);
/// Display-range span between evaluated white and black anchors (B5.10 deepen).
f32 tonemap_curve_output_span(const TonemapCurveParams& params, f32 white_input = 1.f);
bool tonemap_curve_preserves_black(const TonemapCurveParams& params, f32 epsilon = 1e-4f);
/// True when curve knobs are usable before endpoint evaluation (B5.10 deepen).
bool tonemap_curve_params_valid(const TonemapCurveParams& params);
/// True when black/white anchors stay in display range with positive span (B5.10 deepen).
bool tonemap_curve_endpoints_valid(const TonemapCurveEndpoints& endpoints, f32 epsilon = 1e-4f);
/// True when endpoint input anchors are ordered and non-negative (B5.10 deepen).
bool tonemap_curve_endpoint_inputs_valid(const TonemapCurveEndpoints& endpoints, f32 epsilon = 1e-4f);
/// True when white-point input is usable for endpoint evaluation (B5.10 deepen).
bool tonemap_curve_white_input_valid(f32 white_input, f32 epsilon = 1e-4f);
/// Evaluate + validate curve black/white anchors in one call (B5.10 deepen).
bool tonemap_curve_has_valid_endpoints(const TonemapCurveParams& params, f32 white_input = 1.f,
                                       f32 epsilon = 1e-4f);
/// Evaluate curve output at mid-grey for calibration checks (B5.10 deepen).
f32 tonemap_curve_mid_grey_output(const TonemapCurveParams& params, f32 mid_grey = 0.18f);
fuse::math::Vec3 apply_exposure_ev(const fuse::math::Vec3& hdr, f32 ev_stops);

f32 evaluate_reinhard_curve_channel(f32 channel, const ReinhardCurveParams& params);
f32 evaluate_aces_curve_channel(f32 channel, const AcesCurveParams& params);

/// Evaluate the per-channel tonemap curve (dispatches on `params.kind`).
f32 evaluate_tonemap_curve_channel(f32 channel, const TonemapCurveParams& params);
fuse::math::Vec3 apply_tonemap_curve(const fuse::math::Vec3& hdr, const TonemapCurveParams& params);

/// Host-side tonemap curve pass stub (CUDA kernel deferred).
class TonemapCurve {
public:
    void setParams(const TonemapCurveParams& params) { m_params = params; }
    const TonemapCurveParams& params() const { return m_params; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    fuse::math::Vec3 apply(const fuse::math::Vec3& hdr) const;

private:
    TonemapCurveParams m_params{};
    bool m_ready = false;
};

} // namespace fuse::renderer
