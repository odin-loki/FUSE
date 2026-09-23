#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/renderer/postprocess/auto_exposure.hpp>
#include <fuse/renderer/postprocess/bloom.hpp>
#include <fuse/renderer/postprocess/color_grade.hpp>
#include <fuse/renderer/postprocess/dof.hpp>
#include <fuse/renderer/postprocess/motion_blur.hpp>
#include <fuse/renderer/postprocess/tonemap.hpp>
#include <fuse/renderer/postprocess/tonemap_curve.hpp>
#include <fuse/types.hpp>

#include <array>
#include <vector>

namespace fuse::renderer {

enum class PostProcessStage : u8 {
    Bloom = 0,
    ToneMap = 1,
    ColorGrade = 2,
    Count = 3,
};

/// Full-frame post chain passes in execution order (P5 §5.10): HDR spatial passes first (bloom,
/// depth of field, motion blur), then the per-pixel display transform (tone map, grade, grain).
enum class PostPass : u8 {
    Bloom = 0,
    DepthOfField = 1,
    MotionBlur = 2,
    ToneMap = 3,
    ColorGrade = 4,
    FilmGrain = 5,
    Count = 6,
};

inline constexpr u32 kPostPassCount = static_cast<u32>(PostPass::Count);
const char* post_pass_name(PostPass pass);
/// Canonical execution order of `PostStack::processFrame`.
const std::array<PostPass, kPostPassCount>& post_pass_order();

/// Inputs for one full-frame CPU evaluation of the post chain.
struct PostFrameInput {
    const fuse::math::Vec3* hdr = nullptr;       ///< width * height scene-linear HDR colour
    const f32* linear_depth_m = nullptr;         ///< optional linear view depth (enables DoF)
    const fuse::math::Vec2* velocity_px = nullptr; ///< optional pixels/frame velocity (enables motion blur)
    u32 width = 0;
    u32 height = 0;
    u64 frame_seed = 0; ///< film-grain seed; 0 disables grain
};

struct PostFrameStats {
    std::array<PostPass, kPostPassCount> executed{};
    u32 executed_count = 0;
    f32 exposure_ev = 0.f; ///< total exposure applied before the tone-map curve/operator
};

struct PostStackDesc {
    u32 width = 1;
    u32 height = 1;
};

struct PostStackStats {
    bool bloom_ran = false;
    bool tonemap_curve_ran = false;
    bool tonemap_ran = false;
    bool color_grade_ran = false;
    f32 auto_exposure_ev = 0.f;
};

/// Post-processing stack facade — bloom → tone-map → colour grade (B5.10 — P5 §5.10).
/// `processFrame` runs the full CPU reference chain: bloom → DoF → motion blur → tone map →
/// grade → grain. Exposure = manual EV - auto-exposure scene EV + mid-grey calibration bias.
class PostStack {
public:
    void init(const PostStackDesc& desc);
    void destroy();

    bool isReady() const { return m_ready; }
    const PostStackDesc& desc() const { return m_desc; }
    PostStackStats lastStats() const { return m_stats; }

    void setBloomParams(const BloomParams& params);
    void setColorGradeParams(const ColorGradeParams& params);
    void setTonemapCurveParams(const TonemapCurveParams& params);
    void setAutoExposureParams(const AutoExposureParams& params);
    void setDofParams(const DOFParams& params) { m_dof = params; }
    void setMotionBlurParams(const MotionBlurParams& params) { m_motionBlur = params; }
    const DOFParams& dofParams() const { return m_dof; }
    const MotionBlurParams& motionBlurParams() const { return m_motionBlur; }

    Bloom& bloom() { return m_bloom; }
    ToneMap& toneMap() { return m_toneMap; }
    TonemapCurve& tonemapCurve() { return m_tonemapCurve; }
    AutoExposure& autoExposure() { return m_autoExposure; }
    ColorGrade& colorGrade() { return m_colorGrade; }

    const Bloom& bloom() const { return m_bloom; }
    const ToneMap& toneMap() const { return m_toneMap; }
    const TonemapCurve& tonemapCurve() const { return m_tonemapCurve; }
    const AutoExposure& autoExposure() const { return m_autoExposure; }
    const ColorGrade& colorGrade() const { return m_colorGrade; }

    f32 updateAutoExposure(const fuse::math::Vec3* samples, u32 count, f32 delta_seconds);
    f32 updateAutoExposureFromHistogram(const LuminanceHistogram& histogram, f32 delta_seconds);
    void resetAutoExposure();
    void resetAutoExposureTo(f32 ev = 0.f);

    static u32 stageCount() { return static_cast<u32>(PostProcessStage::Count); }
    static const char* stageName(PostProcessStage stage);

    fuse::math::Vec3 processPixel(const fuse::math::Vec3& hdr_input, u64 frame_seed = 0);

    /// Exposure bias that maps scene 0.18 to display-linear 0.18 through the current tone-map curve
    /// and operator (0 when `ColorGradeParams::calibrate_mid_grey` is false).
    f32 midGreyCalibrationEv() const;
    /// Total exposure applied before tone mapping (manual - auto + calibration).
    f32 totalExposureEv() const;

    /// Full-frame CPU reference of the post chain; returns false on invalid input.
    bool processFrame(const PostFrameInput& input, std::vector<fuse::math::Vec3>& out);
    PostFrameStats lastFrameStats() const { return m_frameStats; }

private:
    PostStackDesc m_desc{};
    Bloom m_bloom{};
    ToneMap m_toneMap{};
    TonemapCurve m_tonemapCurve{};
    AutoExposure m_autoExposure{};
    ColorGrade m_colorGrade{};
    DOFParams m_dof{};
    MotionBlurParams m_motionBlur{};
    PostStackStats m_stats{};
    PostFrameStats m_frameStats{};
    bool m_ready = false;
};

} // namespace fuse::renderer
