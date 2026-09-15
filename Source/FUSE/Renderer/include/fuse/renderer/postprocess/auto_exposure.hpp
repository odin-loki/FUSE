#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <vector>

namespace fuse::renderer {

/// Auto-exposure tuning knobs (B5.10 deepen — P5 §5.10 metering stub).
struct AutoExposureParams {
    f32 min_ev = -8.f;
    f32 max_ev = 8.f;
    f32 target_luminance = 0.18f;
    f32 adaptation_speed_up = 3.f;
    f32 adaptation_speed_down = 1.f;
    f32 metering_bias = 0.f;
    f32 ema_alpha_up = 0.15f;
    f32 ema_alpha_down = 0.05f;
    bool use_ema_adaptation = false;
    bool enabled = true;
};

/// Temporal auto-exposure state carried across frames.
struct AutoExposureState {
    f32 current_ev = 0.f;
    f32 measured_luminance = 0.f;
    f32 smoothed_luminance = 0.f;
};

f32 compute_rec709_luminance(const fuse::math::Vec3& rgb);
f32 luminance_to_ev(f32 luminance, f32 target_luminance);
f32 ev_to_luminance(f32 ev, f32 target_luminance);
f32 compute_target_ev(f32 measured_luminance, const AutoExposureParams& params);
f32 clamp_ev(f32 ev, const AutoExposureParams& params);
void reset_auto_exposure_state(AutoExposureState& state);
f32 ema_alpha_for_direction(bool brightening, const AutoExposureParams& params);
bool is_brightening_luminance(f32 measured_luminance, f32 reference_luminance);
f32 ema_blend(f32 previous, f32 measured, f32 alpha);
f32 update_smoothed_luminance(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params);
f32 update_auto_exposure(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                         f32 delta_seconds);
f32 update_auto_exposure_ema(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                             f32 delta_seconds);

/// Log-luminance histogram metering stub (CUDA reduction deferred).
struct LuminanceHistogramParams {
    u32 bin_count = 64;
    f32 min_log_luminance = -8.f;
    f32 max_log_luminance = 8.f;
    f32 metering_percentile = 0.5f;
};

class LuminanceHistogram {
public:
    void reset();
    void init(const LuminanceHistogramParams& params);
    const LuminanceHistogramParams& params() const { return m_params; }

    void accumulate(const fuse::math::Vec3& rgb);
    void accumulateLuminance(f32 luminance);

    bool isEmpty() const { return m_sampleCount == 0; }
    u32 sampleCount() const { return m_sampleCount; }
    static u32 logBinIndex(f32 luminance, const LuminanceHistogramParams& params);
    static f32 binCenterLuminance(u32 bin, const LuminanceHistogramParams& params);
    u32 occupiedBinCount() const;
    f32 averageLuminance() const;
    f32 percentileLuminance(f32 percentile) const;
    f32 meteringLuminance() const;

    static f32 measureFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params);

private:
    LuminanceHistogramParams m_params{};
    std::vector<u32> m_bins{};
    u32 m_sampleCount = 0;
};

/// Batch histogram accumulation helpers (B5.10 deepen).
namespace histogram_util {
void accumulateSamples(LuminanceHistogram& histogram, const fuse::math::Vec3* samples, u32 count);
f32 measurePercentile(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                      f32 percentile);
/// Percentile metering from samples; returns 0 when `count == 0` (B5.10 deepen).
f32 meterFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                     f32 percentile);
} // namespace histogram_util

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
    void reset();

    f32 updateFromSamples(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds);
    f32 updateFromHistogram(const LuminanceHistogram& histogram, f32 delta_seconds);
    f32 updateFromLuminance(f32 measured_luminance, f32 delta_seconds);

private:
    AutoExposureParams m_params{};
    AutoExposureState m_state{};
    bool m_ready = false;
};

} // namespace fuse::renderer
