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
bool auto_exposure_ev_anchor_valid(f32 ev, const AutoExposureParams& params);
void reset_auto_exposure_state(AutoExposureState& state);
/// Reset temporal state while preserving a scene-load EV anchor (B5.10 deepen).
void reset_auto_exposure_state_to(AutoExposureState& state, f32 ev = 0.f);
/// Reset temporal state with EV anchor clamped to `AutoExposureParams` range (B5.10 deepen).
void reset_auto_exposure_state_to_clamped(AutoExposureState& state, f32 ev, const AutoExposureParams& params);
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

/// True when histogram binning and percentile knobs are usable (B5.10 deepen).
bool luminance_histogram_params_valid(const LuminanceHistogramParams& params);
/// True when an explicit percentile knob is in [0, 1] (B5.10 deepen).
bool luminance_histogram_percentile_valid(f32 percentile);
/// True when a histogram bin index is within the configured bin count (B5.10 deepen).
bool luminance_histogram_bin_in_range(u32 bin, const LuminanceHistogramParams& params);

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

void reset_luminance_histogram(LuminanceHistogram& histogram);

/// True when auto-exposure tuning knobs are usable (B5.10 deepen).
bool auto_exposure_params_valid(const AutoExposureParams& params);
/// True when auto-exposure adaptation is enabled and params are valid (B5.10 deepen).
bool auto_exposure_can_adapt(const AutoExposureParams& params);
/// True when a measured luminance can drive adaptation (B5.10 deepen).
bool auto_exposure_can_update_from_luminance(f32 measured_luminance, const AutoExposureParams& params);

/// Batch histogram accumulation helpers (B5.10 deepen).
namespace histogram_util {
/// True when a sample buffer can contribute metering (non-null and non-empty).
bool hasMeteringSamples(const fuse::math::Vec3* samples, u32 count);
/// True when an explicit percentile and histogram params are ready for metering (B5.10 deepen).
bool canMeterPercentile(f32 percentile, const LuminanceHistogramParams& params);
/// True when percentile, params, and sample buffer are ready for percentile metering (B5.10 deepen).
bool canMeterPercentileFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                                   f32 percentile);
/// True when histogram params and sample buffer are ready for percentile metering (B5.10 deepen).
bool canMeterFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params);
/// True when a histogram has accumulated samples (B5.10 deepen).
bool hasMeteringHistogram(const LuminanceHistogram& histogram);
/// True when histogram params and accumulated samples are ready for metering (B5.10 deepen).
bool canMeterFromHistogram(const LuminanceHistogram& histogram);
void accumulateSamples(LuminanceHistogram& histogram, const fuse::math::Vec3* samples, u32 count);
f32 measurePercentile(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                      f32 percentile);
/// Percentile metering from samples; returns 0 when `count == 0` (B5.10 deepen).
f32 meterFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                     f32 percentile);
/// Percentile metering from histogram; returns 0 when empty (B5.10 deepen).
f32 meterFromHistogram(const LuminanceHistogram& histogram, f32 percentile);
} // namespace histogram_util

/// CPU histogram-free exposure meter stub (CUDA reduction deferred).
class ExposureMeter {
public:
    void reset();
    void accumulate(const fuse::math::Vec3& rgb);
    bool isEmpty() const { return m_sampleCount == 0; }
    f32 averageLuminance() const;
    u32 sampleCount() const { return m_sampleCount; }

    static f32 measureAverage(const fuse::math::Vec3* samples, u32 count);

private:
    f64 m_luminanceSum = 0.0;
    u32 m_sampleCount = 0;
};

void reset_exposure_meter(ExposureMeter& meter);
/// True when an exposure meter has accumulated samples (B5.10 deepen).
bool exposure_meter_has_samples(const ExposureMeter& meter);

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
    void resetToEv(f32 ev = 0.f);

    f32 updateFromSamples(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds);
    f32 updateFromHistogram(const LuminanceHistogram& histogram, f32 delta_seconds);
    f32 updateFromLuminance(f32 measured_luminance, f32 delta_seconds);

private:
    AutoExposureParams m_params{};
    AutoExposureState m_state{};
    bool m_ready = false;
};

/// True when a sample buffer can drive auto-exposure adaptation (B5.10 deepen).
bool auto_exposure_can_update_from_samples(const fuse::math::Vec3* samples, u32 count);
/// True when a histogram can drive auto-exposure adaptation (B5.10 deepen).
bool auto_exposure_can_update_from_histogram(const LuminanceHistogram& histogram);
/// Reset temporal auto-exposure state via facade (B5.10 deepen).
void reset_auto_exposure(AutoExposure& exposure);
/// Reset temporal auto-exposure state with clamped EV anchor via facade (B5.10 deepen).
void reset_auto_exposure_to_clamped(AutoExposure& exposure, f32 ev, const AutoExposureParams& params);

} // namespace fuse::renderer
