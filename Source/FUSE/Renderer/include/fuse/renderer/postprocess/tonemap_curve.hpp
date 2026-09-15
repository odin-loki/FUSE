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

TonemapCurveParams make_reinhard_curve_params(const ReinhardCurveParams& reinhard = {});
TonemapCurveParams make_aces_curve_params(const AcesCurveParams& aces = {});

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
