// WP-4.5 GPU post stack: settings resolution and CPU references (see post_gpu_reference.hpp).
#include <fuse/renderer/postprocess/gpu/post_gpu_reference.hpp>

#include <fuse/renderer/look/look_kernels.hpp>
#include <fuse/renderer/look/look_post_chain.hpp>
#include <fuse/renderer/postprocess/post_stack.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::post_gpu {

using math::Vec2;
using math::Vec3;

namespace {

bool isOne(const Vec3& v) { return v.x == 1.f && v.y == 1.f && v.z == 1.f; }
bool isZero(const Vec3& v) { return v.x == 0.f && v.y == 0.f && v.z == 0.f; }

void setGradeStages(PostGpuSettings& s, const Vec3& lift, f32 contrast, f32 saturation, const Vec3& gamma,
                    const Vec3& gain) {
    s.lift = lift;
    s.contrast = contrast;
    s.saturation = saturation;
    s.gamma = gamma;
    s.gain = gain;
    s.gradeLiftContrast = !isZero(lift) || contrast != 1.f;
    s.gradeSaturation = saturation != 1.f;
    s.gradeGammaGain = !isOne(gamma) || !isOne(gain);
}

u32 bit(look::LookEffect e) { return 1u << static_cast<u32>(e); }

} // namespace

PostGpuSettings settings_from_post_stack(const PostStack& stack, u64 frame_seed, f32 delta_seconds) {
    PostGpuSettings s{};
    s.bloomParams = stack.bloom().params();
    s.bloom = s.bloomParams.intensity != 0.f;
    s.dofParams = stack.dofParams();
    s.dof = s.dofParams.enabled;
    s.motionBlurParams = stack.motionBlurParams();
    s.motionBlur = s.motionBlurParams.enabled && s.motionBlurParams.max_samples > 0u;

    const ColorGradeParams& grade = stack.colorGrade().params();
    s.exposureEv = grade.exposure + stack.midGreyCalibrationEv();
    s.autoExposureParams = stack.autoExposure().params();
    s.autoExposure = s.autoExposureParams.enabled;
    s.deltaSeconds = delta_seconds;

    s.curveParams = stack.tonemapCurve().params();
    s.curve = s.curveParams.enabled && tonemap_curve_ready_to_apply(s.curveParams);
    s.toneMapper = stack.toneMap().mapper();
    setGradeStages(s, grade.lift, grade.contrast, grade.saturation, grade.gamma, grade.gain);

    if (grade.vignette != 0.f) {
        s.vignetteMode = PostVignetteMode::Stack;
        s.vignette = grade.vignette;
    }
    if (grade.film_grain > 0.f && frame_seed != 0u) {
        s.grainMode = PostGrainMode::Stack;
        s.grain = grade.film_grain;
    }
    s.frameSeed = frame_seed;
    s.outputSrgb = grade.output_srgb;
    return s;
}

u32 look_unsupported_nodes(const look::LookEffectGraph& graph, const look::LookResolved& look) {
    using look::LookEffect;
    u32 mask = 0;
    for (u32 n = 0; n < graph.size(); ++n) {
        switch (graph.at(n)) {
        case LookEffect::LensDirt:
            mask |= look.lens_dirt.enabled && look.bloom.enabled ? bit(LookEffect::LensDirt) : 0u;
            break;
        case LookEffect::LensFlare:
            mask |= look.lens_flare.enabled && look.bloom.enabled ? bit(LookEffect::LensFlare) : 0u;
            break;
        case LookEffect::Sharpen:
            mask |= look.sharpen.enabled && look.sharpen.sharpness > 0.f ? bit(LookEffect::Sharpen) : 0u;
            break;
        case LookEffect::ChromaticAberration:
            mask |= look.chromatic_aberration.enabled && look.chromatic_aberration.intensity > 0.f
                        ? bit(LookEffect::ChromaticAberration)
                        : 0u;
            break;
        default:
            break;
        }
    }
    return mask;
}

bool settings_from_look(const look::LookEffectGraph& graph, const look::LookResolved& look, u64 frame_seed,
                        f32 delta_seconds, PostGpuSettings& out, u32* unsupported) {
    using look::LookEffect;
    if (unsupported != nullptr) {
        *unsupported = 0u;
    }
    if (!graph.validate().ok()) {
        return false;
    }
    PostGpuSettings s{};
    s.spatialCount = 0;
    s.deltaSeconds = delta_seconds;
    s.frameSeed = frame_seed;
    s.outputSrgb = true;
    s.toneMapper = ToneMapper::Neutral;
    for (u32 n = 0; n < graph.size(); ++n) {
        switch (graph.at(n)) {
        case LookEffect::DepthOfField:
            if (look.dof.enabled) {
                // LookPostChain's mapping (sensor width keeps the DOFParams default).
                s.dof = true;
                s.dofParams.focal_distance = look.dof.focus_distance_m;
                s.dofParams.focal_length = look.dof.focal_length_mm;
                s.dofParams.f_stop = look.dof.f_stop;
                s.dofParams.bokeh_blades = look.dof.bokeh_blades;
                s.dofParams.near_blur = look.dof.near_blur;
                s.dofParams.max_coc_radius_px = look.dof.max_coc_radius_px;
                s.spatialOrder[s.spatialCount++] = PostSpatialPass::DepthOfField;
            }
            break;
        case LookEffect::MotionBlur:
            if (look.motion_blur.enabled) {
                s.motionBlur = true;
                s.motionBlurParams.max_samples = std::max(look.motion_blur.max_samples, 1u);
                s.motionBlurParams.shutter_angle = look.motion_blur.shutter_angle;
                s.motionBlurParams.max_blur_px = look.motion_blur.max_blur_px;
                s.spatialOrder[s.spatialCount++] = PostSpatialPass::MotionBlur;
            }
            break;
        case LookEffect::Bloom:
            if (look.bloom.enabled) {
                s.bloom = true;
                s.bloomParams.threshold = look.bloom.threshold;
                s.bloomParams.knee = look.bloom.knee;
                s.bloomParams.intensity = look.bloom.intensity;
                s.bloomParams.scatter = look.bloom.scatter;
                s.bloomParams.mip_levels = look::kLookBloomLevels;
                s.bloomTint = look.bloom.tint;
                s.spatialOrder[s.spatialCount++] = PostSpatialPass::Bloom;
            }
            break;
        case LookEffect::Exposure: {
            AutoExposureParams ap{};
            ap.enabled = look.exposure.auto_enabled;
            ap.min_ev = std::min(look.exposure.min_ev, look.exposure.max_ev);
            ap.max_ev = std::max(look.exposure.min_ev, look.exposure.max_ev);
            ap.adaptation_speed_up = look.exposure.adapt_speed_up;
            ap.adaptation_speed_down = look.exposure.adapt_speed_down;
            s.autoExposureParams = ap;
            s.autoExposure = ap.enabled;
            s.exposureEv = look.exposure.bias_ev;
            if (look.tonemap.calibrate_mid_grey) {
                s.exposureEv += tone_mapper_mid_grey_calibration_ev(static_cast<ToneMapper>(look.tonemap.op));
            }
            break;
        }
        case LookEffect::ToneMap:
            s.toneMapper = static_cast<ToneMapper>(look.tonemap.op);
            break;
        case LookEffect::ColorGrade:
            s.lut = look.grade.enabled;
            break;
        case LookEffect::Vignette:
            if (look.vignette.enabled && look.vignette.intensity > 0.f) {
                s.vignetteMode = PostVignetteMode::Look;
                s.vignette = look.vignette.intensity;
                s.vignetteFalloff = look.vignette.falloff;
                s.vignetteRoundness = look.vignette.roundness;
                s.vignetteTint = look.vignette.color;
            }
            break;
        case LookEffect::FilmGrain:
            if (look.film_grain.enabled && look.film_grain.intensity > 0.f && frame_seed != 0u) {
                s.grainMode = PostGrainMode::Look;
                s.grain = look.film_grain.intensity;
                s.grainResponse = look.film_grain.response;
            }
            break;
        default:
            break; // AO: exported to the screen-space passes; unsupported nodes: reported below
        }
    }
    if (unsupported != nullptr) {
        *unsupported = look_unsupported_nodes(graph, look);
    }
    out = s;
    return true;
}

u32 bloom_levels(const PostGpuSettings& settings, u32 width, u32 height) {
    return std::min(bloom_level_count(width, height, settings.bloomParams), kPostMaxBloomLevels);
}

void resolve_constants(const PostGpuSettings& s, PostFrameConstants& c) {
    // Only the sections of active passes are written: inactive ones keep the record's defaults, so two
    // front ends that enable the same passes with the same parameters produce the same bytes.
    u32 flags = 0;
    flags |= s.outputSrgb ? kPostFlagSrgb : 0u;
    // Exposure: 2^EV exactly as renderer::apply_exposure_ev computes it.
    c.exposureEv = s.exposureEv;
    c.exposureScale = std::pow(2.f, s.exposureEv);
    if (s.autoExposure) {
        flags |= kPostFlagAutoExposure;
        flags |= s.autoExposureParams.use_ema_adaptation ? kPostFlagAutoEma : 0u;
        const LuminanceHistogramParams& h = s.histogram;
        c.histMinLog = h.min_log_luminance;
        c.histMaxLog = h.max_log_luminance;
        c.histBins = std::min(std::max(h.bin_count, 1u), kPostHistMaxBins);
        c.percentile = h.metering_percentile;
        c.histValid = luminance_histogram_params_valid(h) && h.bin_count <= kPostHistMaxBins ? 1u : 0u;
        const AutoExposureParams& a = s.autoExposureParams;
        c.minEv = a.min_ev;
        c.maxEv = a.max_ev;
        c.targetLuminance = a.target_luminance;
        c.meteringBias = a.metering_bias;
        c.speedUp = a.adaptation_speed_up;
        c.speedDown = a.adaptation_speed_down;
        c.emaUp = a.ema_alpha_up;
        c.emaDown = a.ema_alpha_down;
        c.deltaSeconds = s.deltaSeconds;
        c.adaptValid = auto_exposure_params_valid(a) && s.deltaSeconds > 0.f ? 1u : 0u;
    }
    if (s.bloom) {
        c.bloomThreshold = s.bloomParams.threshold;
        c.bloomKnee = s.bloomParams.knee;
        c.bloomIntensity = s.bloomParams.intensity;
        c.bloomScatter = s.bloomParams.scatter;
        c.bloomTint[0] = s.bloomTint.x;
        c.bloomTint[1] = s.bloomTint.y;
        c.bloomTint[2] = s.bloomTint.z;
        c.bloomTint[3] = 0.f;
    }
    if (s.dof) {
        c.dofFocalDistance = s.dofParams.focal_distance;
        c.dofFocalLength = s.dofParams.focal_length;
        c.dofFStop = s.dofParams.f_stop;
        c.dofSensorWidth = s.dofParams.sensor_width;
        c.dofMaxRadius = s.dofParams.max_coc_radius_px;
        c.dofReach = static_cast<u32>(std::floor(std::max(s.dofParams.max_coc_radius_px, 0.f)));
        flags |= s.dofParams.near_blur ? kPostFlagDofNearBlur : 0u;
    }
    if (s.motionBlur) {
        const MotionBlurParams& m = s.motionBlurParams;
        c.mbSamples = m.max_samples;
        c.mbShutter = std::clamp(m.shutter_angle, 0.f, 360.f) / 360.f;
        c.mbMaxBlur = std::max(m.max_blur_px, 0.f);
        c.mbSoftDepth = m.soft_depth_extent;
        c.mbTile = motion_blur_tile_size(m);
    }
    if (s.curve) {
        flags |= kPostFlagCurve;
        const TonemapCurveParams& p = s.curveParams;
        c.curveKind = static_cast<u32>(p.kind);
        c.filmicA = p.shoulder_strength + 0.22f;
        c.filmicB = p.toe_length + 0.30f;
        c.filmicC = p.toe_strength + 0.10f;
        c.filmicD = p.shoulder_length + 0.20f;
        c.filmicE = 0.01f;
        c.filmicF = p.shoulder_angle + 0.30f;
        // evaluate_filmic_curve's white scale, same expression and order.
        const f32 x = 11.2f;
        const f32 seg = ((x * (c.filmicA * x + c.filmicC * c.filmicB) + c.filmicD * c.filmicE) /
                         (x * (c.filmicA * x + c.filmicB) + c.filmicD * c.filmicF)) -
                        c.filmicE / c.filmicF;
        c.filmicWhiteScale = 1.f / (seg + 1e-8f);
        c.curveGamma = p.gamma;
        c.reinhardWhite = std::max(p.reinhard.white_point, 1e-4f);
        c.reinhardScale = std::pow(2.f, p.reinhard.exposure_bias);
        c.acesContrast = p.aces.contrast;
        c.acesShoulder = p.aces.shoulder;
    }
    c.toneMapper = static_cast<u32>(s.toneMapper);

    if (s.anyGrade()) {
        flags |= kPostFlagGradeClamp;
    }
    if (s.gradeLiftContrast) {
        flags |= kPostFlagGradeLiftContrast;
        c.lift[0] = s.lift.x;
        c.lift[1] = s.lift.y;
        c.lift[2] = s.lift.z;
        c.contrast = s.contrast;
    }
    if (s.gradeSaturation) {
        flags |= kPostFlagGradeSaturation;
        c.saturation = s.saturation;
    }
    if (s.gradeGammaGain) {
        flags |= kPostFlagGradeGammaGain;
        c.invGamma[0] = 1.f / s.gamma.x;
        c.invGamma[1] = 1.f / s.gamma.y;
        c.invGamma[2] = 1.f / s.gamma.z;
        c.gain[0] = s.gain.x;
        c.gain[1] = s.gain.y;
        c.gain[2] = s.gain.z;
    }
    if (s.vignetteMode != PostVignetteMode::None) {
        flags |= s.vignetteMode == PostVignetteMode::Stack ? kPostFlagVignetteStack : kPostFlagVignetteLook;
        c.vignette = s.vignette;
        if (s.vignetteMode == PostVignetteMode::Look) {
            c.vignetteFalloff = s.vignetteFalloff;
            c.vignetteRoundness = s.vignetteRoundness;
            c.vignetteTint[0] = s.vignetteTint.x;
            c.vignetteTint[1] = s.vignetteTint.y;
            c.vignetteTint[2] = s.vignetteTint.z;
        }
    }
    if (s.grainMode != PostGrainMode::None && s.grain > 0.f && s.frameSeed != 0u) {
        flags |= s.grainMode == PostGrainMode::Stack ? kPostFlagGrainStack : kPostFlagGrainLook;
        c.grain = s.grain;
        c.grainResponse = s.grainMode == PostGrainMode::Look ? s.grainResponse : 0.f;
        c.seedLo = static_cast<u32>(s.frameSeed & 0xFFFFFFFFu);
        c.seedHi = static_cast<u32>(s.frameSeed >> 32u);
    }
    c.flags = flags;
}

void spatial_reference(const PostGpuSettings& settings, const Vec3* hdr, const f32* linear_depth, const Vec2* velocity,
                       u32 width, u32 height, std::vector<Vec3>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    out.assign(hdr, hdr + count);
    std::vector<Vec3> scratch;
    for (u32 i = 0; i < settings.spatialCount; ++i) {
        switch (settings.spatialOrder[i]) {
        case PostSpatialPass::Bloom:
            if (settings.bloom) {
                std::vector<Vec3> bloom;
                bloom_image(out.data(), width, height, settings.bloomParams, bloom);
                for (size_t p = 0; p < count; ++p) {
                    const Vec3 b = bloom[p] * settings.bloomParams.intensity;
                    out[p] = out[p] + Vec3{b.x * settings.bloomTint.x, b.y * settings.bloomTint.y, b.z * settings.bloomTint.z};
                }
            }
            break;
        case PostSpatialPass::DepthOfField:
            if (settings.dof && linear_depth != nullptr) {
                dof_pass(out.data(), linear_depth, width, height, settings.dofParams, scratch);
                out.swap(scratch);
            }
            break;
        case PostSpatialPass::MotionBlur:
            if (settings.motionBlur && velocity != nullptr) {
                motion_blur_pass(out.data(), velocity, linear_depth, width, height, settings.motionBlurParams, scratch);
                out.swap(scratch);
            }
            break;
        }
    }
}

Vec3 display_reference(const PostGpuSettings& s, const Vec3& hdr, f32 exposure_scale, u32 x, u32 y, u32 width,
                       u32 height, const look::Lut3D* lut) {
    namespace lk = look::kernels;
    Vec3 c{hdr.x * exposure_scale, hdr.y * exposure_scale, hdr.z * exposure_scale};
    if (s.curve) {
        c = {evaluate_tonemap_curve_channel(c.x, s.curveParams), evaluate_tonemap_curve_channel(c.y, s.curveParams),
             evaluate_tonemap_curve_channel(c.z, s.curveParams)};
    }
    c = apply_tone_map(c, s.toneMapper);
    if (s.anyGrade()) {
        c = apply_tone_map(c, ToneMapper::Neutral); // grade_linear's clamp
    }
    if (s.gradeLiftContrast) {
        c = (c + s.lift - Vec3{0.5f, 0.5f, 0.5f}) * s.contrast + Vec3{0.5f, 0.5f, 0.5f};
    }
    if (s.gradeSaturation) {
        const f32 lum = 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        const Vec3 grey{lum, lum, lum};
        c = grey + (c - grey) * s.saturation;
    }
    if (s.gradeGammaGain) {
        c = Vec3{std::pow(std::max(c.x, 0.f), 1.f / s.gamma.x) * s.gain.x,
                 std::pow(std::max(c.y, 0.f), 1.f / s.gamma.y) * s.gain.y,
                 std::pow(std::max(c.z, 0.f), 1.f / s.gamma.z) * s.gain.z};
    }
    if (s.lut && lut != nullptr && lut->valid()) {
        // look::kernels::GradeLutKernel
        const Vec3 v = lk::max3(c, 0.f);
        const Vec3 base = lk::min3(v, 1.f);
        const Vec3 graded =
            lk::srgb_decode3(lk::saturate3(lk::lut_sample_tetrahedral(lut->data.data(), lut->size, lk::srgb_encode3(base))));
        c = graded + (v - base);
    }
    if (s.vignetteMode == PostVignetteMode::Stack) {
        c = c * vignette_factor(s.vignette, x, y, width, height);
    } else if (s.vignetteMode == PostVignetteMode::Look) {
        const f32 g = lk::vignette_gain(s.vignette, s.vignetteFalloff, s.vignetteRoundness, x, y, width, height);
        c = c * g + s.vignetteTint * (1.f - g);
    }
    if (s.grainMode == PostGrainMode::Stack) {
        c = apply_film_grain(c, s.grain, s.frameSeed, x, y);
    } else if (s.grainMode == PostGrainMode::Look && s.grain > 0.f && s.frameSeed != 0u) {
        const f32 amp = s.grain * (1.f - s.grainResponse * lk::saturate(lk::luminance709(c)));
        const f32 g = lk::grain_noise(s.frameSeed, x, y) * amp;
        c = c + Vec3{g, g, g};
    }
    return finalize_display(c, s.outputSrgb);
}

void histogram_reference(const Vec3* image, u32 count, const LuminanceHistogramParams& params, std::vector<u32>& bins) {
    bins.assign(std::max(params.bin_count, 1u), 0u);
    for (u32 i = 0; i < count; ++i) {
        ++bins[LuminanceHistogram::logBinIndex(compute_rec709_luminance(image[i]), params)];
    }
}

} // namespace fuse::renderer::post_gpu
