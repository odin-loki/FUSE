#include <fuse/renderer/postprocess/auto_exposure.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

namespace {

f32 log_luminance_to_bin(f32 luminance, const LuminanceHistogramParams& params) {
    const f32 safeLum = std::max(luminance, 1e-8f);
    const f32 logLum = std::log2(safeLum);
    const f32 range = params.max_log_luminance - params.min_log_luminance;
    if (range <= 0.f || params.bin_count == 0) {
        return 0.f;
    }
    const f32 normalized = (logLum - params.min_log_luminance) / range;
    return std::clamp(normalized, 0.f, 1.f) * static_cast<f32>(params.bin_count - 1);
}

f32 bin_to_log_luminance(u32 bin, const LuminanceHistogramParams& params) {
    if (params.bin_count <= 1) {
        return params.min_log_luminance;
    }
    const f32 normalized = static_cast<f32>(bin) / static_cast<f32>(params.bin_count - 1);
    const f32 range = params.max_log_luminance - params.min_log_luminance;
    return params.min_log_luminance + normalized * range;
}

} // namespace

f32 compute_rec709_luminance(const fuse::math::Vec3& rgb) {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

bool measured_luminance_valid(f32 luminance, f32 epsilon) {
    return luminance >= epsilon;
}

f32 luminance_to_ev(f32 luminance, f32 target_luminance) {
    const f32 safeLum = std::max(luminance, 1e-8f);
    const f32 safeTarget = std::max(target_luminance, 1e-8f);
    return std::log2(safeLum / safeTarget);
}

f32 ev_to_luminance(f32 ev, f32 target_luminance) {
    const f32 safeTarget = std::max(target_luminance, 1e-8f);
    return safeTarget * std::pow(2.f, ev);
}

f32 compute_target_ev(f32 measured_luminance, const AutoExposureParams& params) {
    return clamp_ev(luminance_to_ev(measured_luminance, params.target_luminance) + params.metering_bias, params);
}

f32 clamp_ev(f32 ev, const AutoExposureParams& params) {
    return std::clamp(ev, params.min_ev, params.max_ev);
}

bool auto_exposure_params_valid(const AutoExposureParams& params) {
    if (params.min_ev > params.max_ev) {
        return false;
    }
    if (params.target_luminance <= 0.f) {
        return false;
    }
    if (params.adaptation_speed_up < 0.f || params.adaptation_speed_down < 0.f) {
        return false;
    }
    if (params.ema_alpha_up < 0.f || params.ema_alpha_up > 1.f) {
        return false;
    }
    if (params.ema_alpha_down < 0.f || params.ema_alpha_down > 1.f) {
        return false;
    }
    return true;
}

bool auto_exposure_can_adapt(const AutoExposureParams& params, f32 delta_seconds) {
    return auto_exposure_params_valid(params) && params.enabled && delta_seconds > 0.f;
}

bool auto_exposure_can_update_from_luminance(f32 luminance, const AutoExposureParams& params, f32 delta_seconds) {
    return measured_luminance_valid(luminance) && auto_exposure_can_adapt(params, delta_seconds);
}

bool auto_exposure_ev_anchor_valid(f32 ev, const AutoExposureParams& params) {
    return ev >= params.min_ev && ev <= params.max_ev;
}

bool auto_exposure_can_update_from_samples(const fuse::math::Vec3* samples, u32 count) {
    return histogram_util::hasMeteringSamples(samples, count);
}

bool auto_exposure_can_update_from_histogram(const LuminanceHistogram& histogram) {
    return histogram_util::canMeterFromHistogram(histogram);
}

f32 ema_alpha_for_direction(bool brightening, const AutoExposureParams& params) {
    return brightening ? params.ema_alpha_up : params.ema_alpha_down;
}

bool is_brightening_luminance(f32 measured_luminance, f32 reference_luminance) {
    return measured_luminance > reference_luminance;
}

f32 ema_blend(f32 previous, f32 measured, f32 alpha) {
    const f32 clampedAlpha = std::clamp(alpha, 0.f, 1.f);
    return clampedAlpha * measured + (1.f - clampedAlpha) * previous;
}

f32 update_smoothed_luminance(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params) {
    const bool brightening = is_brightening_luminance(measured_luminance, state.smoothed_luminance);
    const f32 alpha = ema_alpha_for_direction(brightening, params);
    if (state.smoothed_luminance <= 0.f) {
        state.smoothed_luminance = measured_luminance;
    } else {
        state.smoothed_luminance = ema_blend(state.smoothed_luminance, measured_luminance, alpha);
    }
    return state.smoothed_luminance;
}

f32 update_auto_exposure(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                         f32 delta_seconds) {
    state.measured_luminance = measured_luminance;

    if (!auto_exposure_can_adapt(params, delta_seconds)) {
        return state.current_ev;
    }

    const f32 targetEv = compute_target_ev(measured_luminance, params);
    const f32 deltaEv = targetEv - state.current_ev;
    const f32 speed = deltaEv > 0.f ? params.adaptation_speed_up : params.adaptation_speed_down;
    const f32 maxStep = speed * delta_seconds;
    if (std::fabs(deltaEv) <= maxStep) {
        state.current_ev = targetEv;
    } else {
        state.current_ev += (deltaEv > 0.f ? maxStep : -maxStep);
    }
    state.current_ev = clamp_ev(state.current_ev, params);
    return state.current_ev;
}

f32 update_auto_exposure_ema(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                             f32 delta_seconds) {
    const f32 adaptedLuminance = update_smoothed_luminance(state, measured_luminance, params);
    state.measured_luminance = adaptedLuminance;
    return update_auto_exposure(state, adaptedLuminance, params, delta_seconds);
}

void reset_auto_exposure_state(AutoExposureState& state) {
    reset_auto_exposure_state_to(state, 0.f);
}

void reset_auto_exposure_state_to(AutoExposureState& state, f32 ev) {
    state = {};
    state.current_ev = ev;
}

void reset_auto_exposure_state_to_clamped(AutoExposureState& state, f32 ev, const AutoExposureParams& params) {
    reset_auto_exposure_state_to(state, clamp_ev(ev, params));
}

bool metering_percentile_valid(f32 percentile) {
    return percentile >= 0.f && percentile <= 1.f;
}

bool luminance_histogram_params_valid(const LuminanceHistogramParams& params) {
    if (params.bin_count == 0) {
        return false;
    }
    if (params.max_log_luminance <= params.min_log_luminance) {
        return false;
    }
    return metering_percentile_valid(params.metering_percentile);
}

void reset_luminance_histogram(LuminanceHistogram& histogram) {
    histogram.reset();
}

namespace histogram_util {

bool hasMeteringSamples(const fuse::math::Vec3* samples, u32 count) {
    return samples != nullptr && count > 0;
}

bool canAccumulateFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params) {
    return hasMeteringSamples(samples, count) && luminance_histogram_params_valid(params);
}

bool canMeterFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params) {
    return canAccumulateFromSamples(samples, count, params);
}

bool hasMeteringHistogram(const LuminanceHistogram& histogram) {
    return !histogram.isEmpty();
}

bool canMeterFromHistogram(const LuminanceHistogram& histogram) {
    return hasMeteringHistogram(histogram) && luminance_histogram_params_valid(histogram.params());
}

bool canMeterPercentile(const LuminanceHistogram& histogram, f32 percentile) {
    return metering_percentile_valid(percentile) && canMeterFromHistogram(histogram);
}

bool canMeterPercentileFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                                   f32 percentile) {
    return metering_percentile_valid(percentile) && canMeterFromSamples(samples, count, params);
}

void accumulateSamples(LuminanceHistogram& histogram, const fuse::math::Vec3* samples, u32 count) {
    if (!canAccumulateFromSamples(samples, count, histogram.params())) {
        return;
    }
    for (u32 i = 0; i < count; ++i) {
        histogram.accumulate(samples[i]);
    }
}

f32 measurePercentile(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                      f32 percentile) {
    if (!canMeterPercentileFromSamples(samples, count, params, percentile)) {
        return 0.f;
    }
    LuminanceHistogram histogram;
    histogram.init(params);
    accumulateSamples(histogram, samples, count);
    return histogram.percentileLuminance(percentile);
}

f32 meterFromSamples(const fuse::math::Vec3* samples, u32 count, const LuminanceHistogramParams& params,
                     f32 percentile) {
    if (!canMeterPercentileFromSamples(samples, count, params, percentile)) {
        return 0.f;
    }
    return measurePercentile(samples, count, params, percentile);
}

f32 meterFromHistogram(const LuminanceHistogram& histogram, f32 percentile) {
    if (!canMeterPercentile(histogram, percentile)) {
        return 0.f;
    }
    return histogram.percentileLuminance(percentile);
}

} // namespace histogram_util

void LuminanceHistogram::reset() {
    m_bins.assign(m_params.bin_count, 0u);
    m_sampleCount = 0;
}

void LuminanceHistogram::init(const LuminanceHistogramParams& params) {
    m_params = params;
    if (m_params.bin_count == 0) {
        m_params.bin_count = 1;
    }
    reset();
}

void LuminanceHistogram::accumulateLuminance(f32 luminance) {
    if (m_bins.empty()) {
        init(m_params);
    }
    const u32 clampedBin = logBinIndex(luminance, m_params);
    ++m_bins[clampedBin];
    ++m_sampleCount;
}

void LuminanceHistogram::accumulate(const fuse::math::Vec3& rgb) {
    accumulateLuminance(compute_rec709_luminance(rgb));
}

u32 LuminanceHistogram::logBinIndex(f32 luminance, const LuminanceHistogramParams& params) {
    if (params.bin_count == 0) {
        return 0;
    }
    const u32 bin = static_cast<u32>(std::round(log_luminance_to_bin(luminance, params)));
    return std::min(bin, params.bin_count - 1);
}

f32 LuminanceHistogram::binCenterLuminance(u32 bin, const LuminanceHistogramParams& params) {
    return std::pow(2.f, bin_to_log_luminance(bin, params));
}

u32 LuminanceHistogram::occupiedBinCount() const {
    u32 occupied = 0;
    for (u32 count : m_bins) {
        if (count > 0) {
            ++occupied;
        }
    }
    return occupied;
}

f32 LuminanceHistogram::averageLuminance() const {
    if (m_sampleCount == 0 || m_bins.empty()) {
        return 0.f;
    }

    f64 weightedLog = 0.0;
    for (u32 bin = 0; bin < m_bins.size(); ++bin) {
        weightedLog += static_cast<f64>(m_bins[bin]) * static_cast<f64>(bin_to_log_luminance(bin, m_params));
    }
    const f32 averageLog = static_cast<f32>(weightedLog / static_cast<f64>(m_sampleCount));
    return std::pow(2.f, averageLog);
}

f32 LuminanceHistogram::percentileLuminance(f32 percentile) const {
    if (m_sampleCount == 0 || m_bins.empty()) {
        return 0.f;
    }

    const f32 clampedPercentile = std::clamp(percentile, 0.f, 1.f);
    const u32 targetCount = static_cast<u32>(std::ceil(clampedPercentile * static_cast<f32>(m_sampleCount)));
    const u32 desiredCount = std::max(targetCount, 1u);

    u32 running = 0;
    for (u32 bin = 0; bin < m_bins.size(); ++bin) {
        running += m_bins[bin];
        if (running >= desiredCount) {
            return std::pow(2.f, bin_to_log_luminance(bin, m_params));
        }
    }

    return std::pow(2.f, bin_to_log_luminance(static_cast<u32>(m_bins.size() - 1), m_params));
}

f32 LuminanceHistogram::meteringLuminance() const {
    return percentileLuminance(m_params.metering_percentile);
}

f32 LuminanceHistogram::measureFromSamples(const fuse::math::Vec3* samples, u32 count,
                                           const LuminanceHistogramParams& params) {
    LuminanceHistogram histogram;
    histogram.init(params);
    if (samples == nullptr || count == 0) {
        return 0.f;
    }
    for (u32 i = 0; i < count; ++i) {
        histogram.accumulate(samples[i]);
    }
    return histogram.meteringLuminance();
}

void ExposureMeter::reset() {
    m_luminanceSum = 0.0;
    m_sampleCount = 0;
}

void reset_exposure_meter(ExposureMeter& meter) {
    meter.reset();
}

bool exposure_meter_has_samples(const ExposureMeter& meter) {
    return !meter.isEmpty();
}

bool exposure_meter_can_measure(const ExposureMeter& meter) {
    return exposure_meter_has_samples(meter);
}

void ExposureMeter::accumulate(const fuse::math::Vec3& rgb) {
    m_luminanceSum += static_cast<f64>(compute_rec709_luminance(rgb));
    ++m_sampleCount;
}

f32 ExposureMeter::averageLuminance() const {
    if (m_sampleCount == 0) {
        return 0.f;
    }
    return static_cast<f32>(m_luminanceSum / static_cast<f64>(m_sampleCount));
}

f32 ExposureMeter::measureAverage(const fuse::math::Vec3* samples, u32 count) {
    if (samples == nullptr || count == 0) {
        return 0.f;
    }
    ExposureMeter meter;
    for (u32 i = 0; i < count; ++i) {
        meter.accumulate(samples[i]);
    }
    return meter.averageLuminance();
}

void AutoExposure::init() {
    m_state = {};
    m_ready = true;
}

void AutoExposure::destroy() {
    m_state = {};
    m_ready = false;
}

void AutoExposure::reset() {
    reset_auto_exposure_state(m_state);
}

void AutoExposure::resetToEv(f32 ev) {
    reset_auto_exposure_state_to_clamped(m_state, ev, m_params);
}

f32 AutoExposure::updateFromLuminance(f32 measured_luminance, f32 delta_seconds) {
    if (!measured_luminance_valid(measured_luminance)) {
        return m_state.current_ev;
    }
    if (m_params.use_ema_adaptation) {
        return update_auto_exposure_ema(m_state, measured_luminance, m_params, delta_seconds);
    }
    return update_auto_exposure(m_state, measured_luminance, m_params, delta_seconds);
}

f32 AutoExposure::updateFromHistogram(const LuminanceHistogram& histogram, f32 delta_seconds) {
    if (!auto_exposure_can_update_from_histogram(histogram)) {
        return m_state.current_ev;
    }
    return updateFromLuminance(histogram.meteringLuminance(), delta_seconds);
}

f32 AutoExposure::updateFromSamples(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds) {
    if (!auto_exposure_can_update_from_samples(samples, count)) {
        return m_state.current_ev;
    }
    const f32 average = ExposureMeter::measureAverage(samples, count);
    return updateFromLuminance(average, delta_seconds);
}

void reset_auto_exposure(AutoExposure& exposure) {
    exposure.reset();
}

void reset_auto_exposure_to_clamped(AutoExposure& exposure, f32 ev, const AutoExposureParams& params) {
    exposure.setParams(params);
    exposure.resetToEv(ev);
}

} // namespace fuse::renderer
