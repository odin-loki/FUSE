#include <fuse/renderer/postprocess/post_stack.hpp>

namespace fuse::renderer {

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

fuse::math::Vec3 PostStack::processPixel(const fuse::math::Vec3& hdr_input, u64 frame_seed) {
    m_stats = {};

    const fuse::math::Vec3 bloomed = Bloom::apply(hdr_input, m_bloom.params());
    m_stats.bloom_ran = true;

    const f32 manualEv = m_colorGrade.params().exposure;
    const f32 autoEv = m_autoExposure.params().enabled ? m_autoExposure.currentEv() : 0.f;
    m_stats.auto_exposure_ev = autoEv;
    const fuse::math::Vec3 exposed = bloomed * std::pow(2.f, manualEv + autoEv);

    const fuse::math::Vec3 curved = m_tonemapCurve.apply(exposed);
    m_stats.tonemap_curve_ran = m_tonemapCurve.params().enabled;

    const fuse::math::Vec3 tonemapped = m_toneMap.apply(curved);
    m_stats.tonemap_ran = true;

    ColorGradeParams gradeParams = m_colorGrade.params();
    gradeParams.exposure = 0.f;
    gradeParams.tone_mapper = ToneMapper::Neutral;
    const fuse::math::Vec3 graded = apply_color_grade(tonemapped, gradeParams, frame_seed);
    m_stats.color_grade_ran = true;

    return graded;
}

} // namespace fuse::renderer
