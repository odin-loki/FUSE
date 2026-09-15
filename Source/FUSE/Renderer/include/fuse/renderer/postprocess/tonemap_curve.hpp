#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Filmic S-curve parameters applied before the tone-map operator (B5.10 deepen).
struct TonemapCurveParams {
    f32 toe_strength = 0.f;
    f32 toe_length = 0.5f;
    f32 shoulder_strength = 0.f;
    f32 shoulder_length = 0.5f;
    f32 shoulder_angle = 1.f;
    f32 gamma = 1.f;
    bool enabled = false;
};

/// Evaluate the per-channel filmic curve (Uncharted-2-style stub).
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
