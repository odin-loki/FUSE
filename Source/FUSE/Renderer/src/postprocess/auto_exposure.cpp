#include <fuse/renderer/postprocess/auto_exposure.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer {

f32 compute_rec709_luminance(const fuse::math::Vec3& rgb) {
    return 0.2126f * rgb.x + 0.7152f * rgb.y + 0.0722f * rgb.z;
}

f32 luminance_to_ev(f32 luminance, f32 target_luminance) {
    const f32 safeLum = std::max(luminance, 1e-8f);
    const f32 safeTarget = std::max(target_luminance, 1e-8f);
    return std::log2(safeLum / safeTarget);
}

f32 clamp_ev(f32 ev, const AutoExposureParams& params) {
    return std::clamp(ev, params.min_ev, params.max_ev);
}

f32 update_auto_exposure(AutoExposureState& state, f32 measured_luminance, const AutoExposureParams& params,
                         f32 delta_seconds) {
    state.measured_luminance = measured_luminance;

    if (!params.enabled) {
        return state.current_ev;
    }

    const f32 targetEv = clamp_ev(luminance_to_ev(measured_luminance, params.target_luminance) + params.metering_bias,
                                  params);
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

void ExposureMeter::reset() {
    m_luminanceSum = 0.0;
    m_sampleCount = 0;
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

f32 AutoExposure::updateFromLuminance(f32 measured_luminance, f32 delta_seconds) {
    return update_auto_exposure(m_state, measured_luminance, m_params, delta_seconds);
}

f32 AutoExposure::updateFromSamples(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds) {
    const f32 average = ExposureMeter::measureAverage(samples, count);
    return updateFromLuminance(average, delta_seconds);
}

} // namespace fuse::renderer
