// FUSE Relight post: per-channel lift / gamma / gain.
//
//   out = pow(max(value * gain + lift, 0), 1 / gamma)
//
// Negatives are cleared before the power so the result stays non-negative. Values above 1 are left
// above 1 (HDR is not clamped to display range). A non-finite lift, gain, or value yields 0 on that
// channel. A gamma that is non-finite or <= 0 is treated as 1, which is the clamped value above.
// Gamma of exactly 1 returns that clamped value directly so a neutral grade is an exact identity on
// finite non-negative inputs.
#pragma once

#include <fuse/math/vec.hpp>

#include <cmath>

namespace fuse::relight::render::post {

/// Lift (offset), gamma, and gain applied independently to each channel.
struct LiftGammaGain {
    math::Vec3 lift{0.f, 0.f, 0.f};
    math::Vec3 gamma{1.f, 1.f, 1.f};
    math::Vec3 gain{1.f, 1.f, 1.f};
};

/// `pow(max(value * gain + lift, 0), 1 / gamma)` per channel. See the file comment for the
/// non-finite and non-positive gamma rules.
[[nodiscard]] inline math::Vec3 applyLiftGammaGain(const math::Vec3& value, const LiftGammaGain& grade) {
    const auto channel = [](float v, float lift, float gamma, float gain) {
        if (!std::isfinite(v) || !std::isfinite(lift) || !std::isfinite(gain)) {
            return 0.f;
        }
        const float biased = v * gain + lift;
        const float clamped = biased > 0.f ? biased : 0.f;
        if (!std::isfinite(gamma) || gamma <= 0.f || gamma == 1.f) {
            return clamped;
        }
        return std::pow(clamped, 1.f / gamma);
    };
    return {channel(value.x, grade.lift.x, grade.gamma.x, grade.gain.x),
            channel(value.y, grade.lift.y, grade.gamma.y, grade.gain.y),
            channel(value.z, grade.lift.z, grade.gamma.z, grade.gain.z)};
}

} // namespace fuse::relight::render::post
