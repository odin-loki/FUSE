#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Auto-exposure tuning knobs (B5.10 deepen — P5 §5.10 metering stub).
struct AutoExposureParams {
    f32 min_ev = -8.f;
    f32 max_ev = 8.f;
    f32 target_luminance = 0.18f;
    f32 adaptation_speed_up = 3.f;
    f32 adaptation_speed_down = 1.f;
    f32 metering_bias = 0.f;
    bool enabled = true;
};

/// Temporal auto-exposure state carried across frames.
struct AutoExposureState {
    f32 current_ev = 0.f;
    f32 measured_luminance = 0.f;
};

f32 compute_rec709_luminance(const fuse::math::Vec3& rgb);
f32 luminance_to_ev(f32 luminance, f32 target_luminance);
f32 clamp_ev(f32 ev, const AutoExposureParams& params);
f32 update_auto_exposure(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                         f32 delta_seconds);

/// CPU histogram-free exposure meter stub (CUDA reduction deferred).
class ExposureMeter {
public:
    void reset();
    void accumulate(const fuse::math::Vec3& rgb);
    f32 averageLuminance() const;
    u32 sampleCount() const { return m_sampleCount; }

    static f32 measureAverage(const fuse::math::Vec3* samples, u32 count);

private:
    f64 m_luminanceSum = 0.0;
    u32 m_sampleCount = 0;
};

/// Host-side auto-exposure pass stub (CUDA histogram deferred).
class AutoExposure {
public:
    void setParams(const AutoExposureParams& params) { m_params = params; }
    const AutoExposureParams& params() const { return m_params; }
    const AutoExposureState& state() const { return m_state; }
    f32 currentEv() const { return m_state.current_ev; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    f32 updateFromSamples(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds);
    f32 updateFromLuminance(f32 measured_luminance, f32 delta_seconds);

private:
    AutoExposureParams m_params{};
    AutoExposureState m_state{};
    bool m_ready = false;
};

} // namespace fuse::renderer
