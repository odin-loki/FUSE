#include <fuse/renderer/postprocess/post_stack.hpp>

#include <cmath>

namespace fuse::renderer {

const char* post_pass_name(PostPass pass) {
    switch (pass) {
    case PostPass::Bloom:
        return "bloom";
    case PostPass::DepthOfField:
        return "dof";
    case PostPass::MotionBlur:
        return "motion_blur";
    case PostPass::ToneMap:
        return "tonemap";
    case PostPass::ColorGrade:
        return "color_grade";
    case PostPass::FilmGrain:
        return "film_grain";
    default:
        return "unknown";
    }
}

const std::array<PostPass, kPostPassCount>& post_pass_order() {
    static const std::array<PostPass, kPostPassCount> order = {PostPass::Bloom,    PostPass::DepthOfField,
                                                               PostPass::MotionBlur, PostPass::ToneMap,
                                                               PostPass::ColorGrade, PostPass::FilmGrain};
    return order;
}

const char* PostStack::stageName(PostProcessStage stage) {
    switch (stage) {
    case PostProcessStage::Bloom:
        return "bloom";
    case PostProcessStage::ToneMap:
        return "tonemap";
    case PostProcessStage::ColorGrade:
        return "color_grade";
    default:
        return "unknown";
    }
}

void PostStack::init(const PostStackDesc& desc) {
    m_desc = desc;
    m_bloom.init();
    m_toneMap.init();
    m_tonemapCurve.init();
    m_autoExposure.init();
    m_colorGrade.init();
    m_ready = true;
    m_stats = {};
}

void PostStack::destroy() {
    m_bloom.destroy();
    m_toneMap.destroy();
    m_tonemapCurve.destroy();
    m_autoExposure.destroy();
    m_colorGrade.destroy();
    m_ready = false;
    m_stats = {};
}

void PostStack::setBloomParams(const BloomParams& params) {
    m_bloom.setParams(params);
}

void PostStack::setColorGradeParams(const ColorGradeParams& params) {
    m_colorGrade.setParams(params);
    m_toneMap.setMapper(params.tone_mapper);
}

void PostStack::setTonemapCurveParams(const TonemapCurveParams& params) {
    m_tonemapCurve.setParams(params);
}

void PostStack::setAutoExposureParams(const AutoExposureParams& params) {
    m_autoExposure.setParams(params);
}

f32 PostStack::updateAutoExposure(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds) {
    return m_autoExposure.updateFromSamples(samples, count, delta_seconds);
}

f32 PostStack::updateAutoExposureFromHistogram(const LuminanceHistogram& histogram, f32 delta_seconds) {
    return m_autoExposure.updateFromHistogram(histogram, delta_seconds);
}

void PostStack::resetAutoExposure() {
    m_autoExposure.reset();
}

void PostStack::resetAutoExposureTo(f32 ev) {
    m_autoExposure.resetToEv(ev);
}

f32 PostStack::midGreyCalibrationEv() const {
    if (!m_colorGrade.params().calibrate_mid_grey) {
        return 0.f;
    }
    if (!m_tonemapCurve.params().enabled) {
        return tone_mapper_mid_grey_calibration_ev(m_toneMap.mapper());
    }
    const auto evaluate = [this](f32 ev) {
        const f32 x = kSceneMidGrey * std::exp2(ev);
        return m_toneMap.apply(m_tonemapCurve.apply({x, x, x})).x;
    };
    f32 lo = -16.f;
    f32 hi = 16.f;
    if (evaluate(lo) > kDisplayMidGrey || evaluate(hi) < kDisplayMidGrey) {
        return 0.f;
    }
    for (u32 i = 0; i < 64u; ++i) {
        const f32 mid = 0.5f * (lo + hi);
        if (evaluate(mid) < kDisplayMidGrey) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

f32 PostStack::totalExposureEv() const {
    const f32 manualEv = m_colorGrade.params().exposure;
    // Auto exposure meters the scene EV relative to the target luminance; compensation is its negation.
    const f32 autoEv = m_autoExposure.params().enabled ? m_autoExposure.currentEv() : 0.f;
    return manualEv - autoEv + midGreyCalibrationEv();
}

fuse::math::Vec3 PostStack::processPixel(const fuse::math::Vec3& hdr_input, u64 frame_seed) {
    m_stats = {};

    const fuse::math::Vec3 bloomed = Bloom::apply(hdr_input, m_bloom.params());
    m_stats.bloom_ran = true;

    const f32 autoEv = m_autoExposure.params().enabled ? m_autoExposure.currentEv() : 0.f;
    m_stats.auto_exposure_ev = autoEv;
    const fuse::math::Vec3 exposed = apply_exposure_ev(bloomed, totalExposureEv());

    const fuse::math::Vec3 curved = m_tonemapCurve.apply(exposed);
    m_stats.tonemap_curve_ran = m_tonemapCurve.params().enabled;

    const fuse::math::Vec3 tonemapped = m_toneMap.apply(curved);
    m_stats.tonemap_ran = true;

    ColorGradeParams gradeParams = m_colorGrade.params();
    gradeParams.exposure = 0.f;
    gradeParams.tone_mapper = ToneMapper::Neutral;
    gradeParams.calibrate_mid_grey = false;
    const fuse::math::Vec3 graded = apply_color_grade(tonemapped, gradeParams, frame_seed);
    m_stats.color_grade_ran = true;

    return graded;
}

bool PostStack::processFrame(const PostFrameInput& input, std::vector<fuse::math::Vec3>& out) {
    m_frameStats = {};
    m_stats = {};
    out.clear();
    if (input.hdr == nullptr || input.width == 0u || input.height == 0u) {
        return false;
    }
    const auto record = [this](PostPass pass) { m_frameStats.executed[m_frameStats.executed_count++] = pass; };
    const u32 width = input.width;
    const u32 height = input.height;
    const size_t count = static_cast<size_t>(width) * height;

    std::vector<fuse::math::Vec3> hdr;
    bloom_composite(input.hdr, width, height, m_bloom.params(), hdr);
    record(PostPass::Bloom);
    m_stats.bloom_ran = true;

    std::vector<fuse::math::Vec3> scratch;
    if (input.linear_depth_m != nullptr && m_dof.enabled) {
        dof_pass(hdr.data(), input.linear_depth_m, width, height, m_dof, scratch);
        hdr.swap(scratch);
        record(PostPass::DepthOfField);
    }
    if (input.velocity_px != nullptr && m_motionBlur.enabled) {
        motion_blur_pass(hdr.data(), input.velocity_px, input.linear_depth_m, width, height, m_motionBlur, scratch);
        hdr.swap(scratch);
        record(PostPass::MotionBlur);
    }

    const f32 exposureEv = totalExposureEv();
    m_frameStats.exposure_ev = exposureEv;
    m_stats.auto_exposure_ev = m_autoExposure.params().enabled ? m_autoExposure.currentEv() : 0.f;

    ColorGradeParams gradeParams = m_colorGrade.params();
    gradeParams.exposure = 0.f;
    gradeParams.tone_mapper = ToneMapper::Neutral;
    gradeParams.calibrate_mid_grey = false;
    const bool grain = gradeParams.film_grain > 0.f && input.frame_seed != 0u;

    out.resize(count);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const size_t index = static_cast<size_t>(y) * width + x;
            const fuse::math::Vec3 exposed = apply_exposure_ev(hdr[index], exposureEv);
            const fuse::math::Vec3 tonemapped = m_toneMap.apply(m_tonemapCurve.apply(exposed));
            fuse::math::Vec3 graded = grade_linear(tonemapped, gradeParams);
            graded = graded * vignette_factor(gradeParams.vignette, x, y, width, height);
            graded = apply_film_grain(graded, gradeParams.film_grain, input.frame_seed, x, y);
            out[index] = finalize_display(graded, gradeParams.output_srgb);
        }
    }
    record(PostPass::ToneMap);
    record(PostPass::ColorGrade);
    if (grain) {
        record(PostPass::FilmGrain);
    }
    m_stats.tonemap_curve_ran = m_tonemapCurve.params().enabled;
    m_stats.tonemap_ran = true;
    m_stats.color_grade_ran = true;
    return true;
}

} // namespace fuse::renderer
