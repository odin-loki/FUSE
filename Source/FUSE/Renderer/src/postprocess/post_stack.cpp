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
    m_colorGrade.init();
    m_ready = true;
    m_stats = {};
}

void PostStack::destroy() {
    m_bloom.destroy();
    m_toneMap.destroy();
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

fuse::math::Vec3 PostStack::processPixel(const fuse::math::Vec3& hdr_input, u64 frame_seed) {
    m_stats = {};

    const fuse::math::Vec3 bloomed = Bloom::apply(hdr_input, m_bloom.params());
    m_stats.bloom_ran = true;

    const fuse::math::Vec3 exposed = bloomed * std::pow(2.f, m_colorGrade.params().exposure);
    const fuse::math::Vec3 tonemapped = m_toneMap.apply(exposed);
    m_stats.tonemap_ran = true;

    ColorGradeParams gradeParams = m_colorGrade.params();
    gradeParams.exposure = 0.f;
    gradeParams.tone_mapper = ToneMapper::Neutral;
    const fuse::math::Vec3 graded = apply_color_grade(tonemapped, gradeParams, frame_seed);
    m_stats.color_grade_ran = true;

    return graded;
}

} // namespace fuse::renderer
