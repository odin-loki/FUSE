#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

enum class PostProcessStage : u8 {
    Bloom = 0,
    ToneMap = 1,
    ColorGrade = 2,
    Count = 3,
};

struct PostStackDesc {
    u32 width = 1;
    u32 height = 1;
};

struct PostStackStats {
    bool bloom_ran = false;
    bool tonemap_ran = false;
    bool color_grade_ran = false;
};

/// Post-processing stack facade — bloom → tone-map → colour grade (B5.10 — P5 §5.10).
class PostStack {
public:
    void init(const PostStackDesc& desc);
    void destroy();

    bool isReady() const { return m_ready; }
    const PostStackDesc& desc() const { return m_desc; }
    PostStackStats lastStats() const { return m_stats; }

    void setBloomParams(const BloomParams& params);
    void setColorGradeParams(const ColorGradeParams& params);

    Bloom& bloom() { return m_bloom; }
    ToneMap& toneMap() { return m_toneMap; }
    ColorGrade& colorGrade() { return m_colorGrade; }

    const Bloom& bloom() const { return m_bloom; }
    const ToneMap& toneMap() const { return m_toneMap; }
    const ColorGrade& colorGrade() const { return m_colorGrade; }

    static u32 stageCount() { return static_cast<u32>(PostProcessStage::Count); }
    static const char* stageName(PostProcessStage stage);

    fuse::math::Vec3 processPixel(const fuse::math::Vec3& hdr_input, u64 frame_seed = 0);

private:
    PostStackDesc m_desc{};
    Bloom m_bloom{};
    ToneMap m_toneMap{};
    ColorGrade m_colorGrade{};
    PostStackStats m_stats{};
    bool m_ready = false;
};

} // namespace fuse::renderer
