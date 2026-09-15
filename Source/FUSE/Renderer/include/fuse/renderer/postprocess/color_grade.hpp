#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Colour grading parameters (B5.10 — P5 §5.10).
struct ColorGradeParams {
    ToneMapper tone_mapper = ToneMapper::ACES;
    f32 exposure = 0.f;
    f32 contrast = 1.f;
    f32 saturation = 1.f;
    fuse::math::Vec3 lift{};
    fuse::math::Vec3 gamma{1.f, 1.f, 1.f};
    fuse::math::Vec3 gain{1.f, 1.f, 1.f};
    f32 vignette = 0.3f;
    f32 film_grain = 0.02f;
    bool output_srgb = true;
};

fuse::math::Vec3 linear_to_srgb(const fuse::math::Vec3& linear);
fuse::math::Vec3 apply_color_grade(const fuse::math::Vec3& input, const ColorGradeParams& params,
                                   u64 frame_seed = 0);

/// Host-side colour-grade pass stub (CUDA kernel deferred).
class ColorGrade {
public:
    void setParams(const ColorGradeParams& params) { m_params = params; }
    const ColorGradeParams& params() const { return m_params; }
    bool isReady() const { return m_ready; }
    void init();
    void destroy();

    fuse::math::Vec3 apply(const fuse::math::Vec3& input, u64 frame_seed = 0) const;

private:
    ColorGradeParams m_params{};
    bool m_ready = false;
};

} // namespace fuse::renderer
