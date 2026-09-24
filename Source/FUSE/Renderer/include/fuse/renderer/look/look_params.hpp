#pragma once

// FUSE Look system — effect nodes and their typed parameter schema.
//
// Every name here (node keys, parameter keys, the `.fuselook` schema) is FUSE's own. The look system
// is an engine-native feature; it does not read, map or mirror any third-party injector's presets,
// parameter identifiers or shader files (see docs/research/upscaling-framegen-and-post-injectors.md
// §2.3 / §4.2).
//
// Parameters live in a flat, fixed-size `LookParamBlock` (3 float slots per parameter, so colours fit
// and blending is a plain loop) — evaluation and blending never allocate.

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <array>
#include <string_view>

namespace fuse::renderer::look {

/// Effect nodes of the look graph. Names (camelCase) are the `.fuselook` keys.
enum class LookEffect : u8 {
    AmbientOcclusion = 0, ///< SSAO / SSIL strength (executed by the screen-space passes, pre-upscale).
    DepthOfField,
    MotionBlur,
    Bloom,
    LensDirt,
    LensFlare,
    Exposure,
    ToneMap,
    ColorGrade, ///< White balance, lift/gamma/gain, contrast, saturation, curves, 3D LUT.
    Sharpen,
    ChromaticAberration,
    Vignette,
    FilmGrain,
    OutputTransform, ///< sRGB / HDR10 PQ (ST 2084) / scRGB encode.
    Count,
};
inline constexpr u32 kLookEffectCount = static_cast<u32>(LookEffect::Count);

/// Research §4.2 stage tags.
enum class LookStage : u8 { PreUpscale = 0, PostUpscaleHdr = 1, Display = 2, Output = 3 };

/// Signal domain flowing between nodes.
enum class LookDomain : u8 {
    SceneHdr = 0,      ///< scene-referred linear HDR
    DisplayLinear = 1, ///< display-referred linear (1.0 = SDR white / HDR paper white)
    Encoded = 2,       ///< output-encoded (sRGB OETF, PQ, scRGB)
    Any = 3,           ///< node accepts SceneHdr or DisplayLinear and preserves it
};

struct LookEffectInfo {
    LookEffect effect;
    const char* key; ///< `.fuselook` key
    LookStage stage;
    LookDomain input;
    LookDomain output;
    bool external; ///< parameters are exported to another pass (not executed by the look chain)
};

const LookEffectInfo& look_effect_info(LookEffect effect);
const char* look_effect_key(LookEffect effect);
bool look_effect_from_key(std::string_view key, LookEffect& out);

enum class LookParamType : u8 { Float = 0, Int = 1, Bool = 2, Color = 3, Enum = 4 };

/// How a parameter blends between profiles.
enum class LookBlendMode : u8 {
    Lerp = 0,    ///< linear (scalars, colours); ints round to nearest after blending
    LogLerp = 1, ///< geometric (distances, f-stops, nits); falls back to Lerp for non-positive values
    Step = 2,    ///< bools / enums: switch to the target once its weight reaches 0.5 (argmax for sums)
};

// X(id, effect, key, type, d0, d1, d2, min, max, blend)
#define FUSE_LOOK_PARAM_LIST(X)                                                                          \
    X(AoEnabled, AmbientOcclusion, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                 \
    X(AoSsaoStrength, AmbientOcclusion, "ssaoStrength", Float, 1, 0, 0, 0, 4, Lerp)                      \
    X(AoSsilStrength, AmbientOcclusion, "ssilStrength", Float, 1, 0, 0, 0, 4, Lerp)                      \
    X(DofEnabled, DepthOfField, "enabled", Bool, 0, 0, 0, 0, 1, Step)                                    \
    X(DofFocusDistanceM, DepthOfField, "focusDistanceM", Float, 10, 0, 0, 0.05f, 10000, LogLerp)         \
    X(DofFocalLengthMm, DepthOfField, "focalLengthMm", Float, 50, 0, 0, 4, 1200, LogLerp)                \
    X(DofFStop, DepthOfField, "fStop", Float, 2.8f, 0, 0, 0.7f, 64, LogLerp)                             \
    X(DofBokehBlades, DepthOfField, "bokehBlades", Int, 6, 0, 0, 0, 16, Step)                            \
    X(DofMaxCocRadiusPx, DepthOfField, "maxCocRadiusPx", Float, 32, 0, 0, 0, 64, Lerp)                   \
    X(DofNearBlur, DepthOfField, "nearBlur", Bool, 1, 0, 0, 0, 1, Step)                                  \
    X(MbEnabled, MotionBlur, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                       \
    X(MbShutterAngle, MotionBlur, "shutterAngle", Float, 180, 0, 0, 0, 360, Lerp)                        \
    X(MbMaxSamples, MotionBlur, "maxSamples", Int, 16, 0, 0, 1, 64, Lerp)                                \
    X(MbMaxBlurPx, MotionBlur, "maxBlurPx", Float, 32, 0, 0, 0, 128, Lerp)                               \
    X(BloomEnabled, Bloom, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                         \
    X(BloomThreshold, Bloom, "threshold", Float, 1, 0, 0, 0, 64, Lerp)                                   \
    X(BloomKnee, Bloom, "knee", Float, 0.5f, 0, 0, 0, 16, Lerp)                                          \
    X(BloomIntensity, Bloom, "intensity", Float, 0.05f, 0, 0, 0, 10, Lerp)                               \
    X(BloomScatter, Bloom, "scatter", Float, 0.7f, 0, 0, 0, 1, Lerp)                                     \
    X(BloomTint, Bloom, "tint", Color, 1, 1, 1, 0, 16, Lerp)                                             \
    X(DirtEnabled, LensDirt, "enabled", Bool, 0, 0, 0, 0, 1, Step)                                       \
    X(DirtIntensity, LensDirt, "intensity", Float, 0.5f, 0, 0, 0, 20, Lerp)                              \
    X(DirtTint, LensDirt, "tint", Color, 1, 1, 1, 0, 16, Lerp)                                           \
    X(FlareEnabled, LensFlare, "enabled", Bool, 0, 0, 0, 0, 1, Step)                                     \
    X(FlareIntensity, LensFlare, "intensity", Float, 0.5f, 0, 0, 0, 10, Lerp)                            \
    X(FlareThreshold, LensFlare, "threshold", Float, 0, 0, 0, 0, 64, Lerp)                               \
    X(FlareGhostCount, LensFlare, "ghostCount", Int, 4, 0, 0, 0, 8, Step)                                \
    X(FlareGhostSpacing, LensFlare, "ghostSpacing", Float, 0.4f, 0, 0, 0, 1, Lerp)                       \
    X(FlareHaloRadius, LensFlare, "haloRadius", Float, 0.45f, 0, 0, 0, 1, Lerp)                          \
    X(FlareHaloThickness, LensFlare, "haloThickness", Float, 0.08f, 0, 0, 0.001f, 0.5f, Lerp)            \
    X(FlareHaloIntensity, LensFlare, "haloIntensity", Float, 0.5f, 0, 0, 0, 10, Lerp)                    \
    X(FlareChromaticShift, LensFlare, "chromaticShift", Float, 0.005f, 0, 0, 0, 0.05f, Lerp)             \
    X(FlareTint, LensFlare, "tint", Color, 1, 1, 1, 0, 16, Lerp)                                         \
    X(ExpBiasEv, Exposure, "biasEv", Float, 0, 0, 0, -16, 16, Lerp)                                      \
    X(ExpAutoEnabled, Exposure, "autoEnabled", Bool, 1, 0, 0, 0, 1, Step)                                \
    X(ExpMinEv, Exposure, "minEv", Float, -8, 0, 0, -24, 24, Lerp)                                       \
    X(ExpMaxEv, Exposure, "maxEv", Float, 8, 0, 0, -24, 24, Lerp)                                        \
    X(ExpAdaptSpeedUp, Exposure, "adaptSpeedUp", Float, 3, 0, 0, 0, 50, Lerp)                            \
    X(ExpAdaptSpeedDown, Exposure, "adaptSpeedDown", Float, 1, 0, 0, 0, 50, Lerp)                        \
    X(TmOperator, ToneMap, "operator", Enum, 0, 0, 0, 0, 3, Step)                                        \
    X(TmCalibrateMidGrey, ToneMap, "calibrateMidGrey", Bool, 1, 0, 0, 0, 1, Step)                        \
    X(GradeEnabled, ColorGrade, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                    \
    X(GradeTemperatureK, ColorGrade, "temperatureK", Float, 6504, 0, 0, 1667, 25000, Lerp)               \
    X(GradeTint, ColorGrade, "tint", Float, 0, 0, 0, -1, 1, Lerp)                                        \
    X(GradeLift, ColorGrade, "lift", Color, 0, 0, 0, -1, 1, Lerp)                                        \
    X(GradeGamma, ColorGrade, "gamma", Color, 1, 1, 1, 0.1f, 10, Lerp)                                   \
    X(GradeGain, ColorGrade, "gain", Color, 1, 1, 1, 0, 10, Lerp)                                        \
    X(GradeContrast, ColorGrade, "contrast", Float, 1, 0, 0, 0, 4, Lerp)                                 \
    X(GradeContrastPivot, ColorGrade, "contrastPivot", Float, 0.18f, 0, 0, 0.001f, 1, Lerp)              \
    X(GradeSaturation, ColorGrade, "saturation", Float, 1, 0, 0, 0, 4, Lerp)                             \
    X(GradeCurve0, ColorGrade, "curve0", Color, 0, 0, 0, 0, 1, Lerp)                                     \
    X(GradeCurve1, ColorGrade, "curve1", Color, 0.25f, 0.25f, 0.25f, 0, 1, Lerp)                         \
    X(GradeCurve2, ColorGrade, "curve2", Color, 0.5f, 0.5f, 0.5f, 0, 1, Lerp)                            \
    X(GradeCurve3, ColorGrade, "curve3", Color, 0.75f, 0.75f, 0.75f, 0, 1, Lerp)                         \
    X(GradeCurve4, ColorGrade, "curve4", Color, 1, 1, 1, 0, 1, Lerp)                                     \
    X(GradeLutStrength, ColorGrade, "lutStrength", Float, 1, 0, 0, 0, 1, Lerp)                           \
    X(SharpenEnabled, Sharpen, "enabled", Bool, 0, 0, 0, 0, 1, Step)                                     \
    X(SharpenSharpness, Sharpen, "sharpness", Float, 0.5f, 0, 0, 0, 1, Lerp)                             \
    X(CaEnabled, ChromaticAberration, "enabled", Bool, 0, 0, 0, 0, 1, Step)                              \
    X(CaIntensity, ChromaticAberration, "intensity", Float, 0.003f, 0, 0, 0, 0.1f, Lerp)                 \
    X(VignetteEnabled, Vignette, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                   \
    X(VignetteIntensity, Vignette, "intensity", Float, 0.3f, 0, 0, 0, 1, Lerp)                           \
    X(VignetteFalloff, Vignette, "falloff", Float, 2, 0, 0, 0.1f, 8, Lerp)                               \
    X(VignetteRoundness, Vignette, "roundness", Float, 0, 0, 0, 0, 1, Lerp)                              \
    X(VignetteColor, Vignette, "color", Color, 0, 0, 0, 0, 1, Lerp)                                      \
    X(GrainEnabled, FilmGrain, "enabled", Bool, 1, 0, 0, 0, 1, Step)                                     \
    X(GrainIntensity, FilmGrain, "intensity", Float, 0.02f, 0, 0, 0, 1, Lerp)                            \
    X(GrainResponse, FilmGrain, "response", Float, 0, 0, 0, 0, 1, Lerp)                                  \
    X(OutPaperWhiteNits, OutputTransform, "paperWhiteNits", Float, 200, 0, 0, 80, 1000, LogLerp)         \
    X(OutPeakNits, OutputTransform, "peakNits", Float, 1000, 0, 0, 100, 10000, LogLerp)

enum class LookParam : u16 {
#define FUSE_LOOK_PARAM_ENUM(id, effect, key, type, d0, d1, d2, mn, mx, blend) id,
    FUSE_LOOK_PARAM_LIST(FUSE_LOOK_PARAM_ENUM)
#undef FUSE_LOOK_PARAM_ENUM
        Count
};
inline constexpr u32 kLookParamCount = static_cast<u32>(LookParam::Count);
/// Every parameter owns 3 consecutive float slots (scalars use slot 0).
inline constexpr u32 kLookSlotsPerParam = 3;
inline constexpr u32 kLookSlotCount = kLookParamCount * kLookSlotsPerParam;

struct LookParamInfo {
    LookParam id;
    LookEffect effect;
    const char* key; ///< key inside the node's `.fuselook` object
    LookParamType type;
    f32 def[3];
    f32 min;
    f32 max;
    LookBlendMode blend;
};

const LookParamInfo& look_param_info(LookParam param);
inline const LookParamInfo& look_param_info(u32 index) { return look_param_info(static_cast<LookParam>(index)); }
/// Parameter by (node, key); false when unknown.
bool look_param_find(LookEffect effect, std::string_view key, LookParam& out);
/// Number of components (1, or 3 for colours).
inline u32 look_param_components(LookParamType type) { return type == LookParamType::Color ? 3u : 1u; }

/// Tone-map operator enum values (`toneMap.operator`), same order as renderer::ToneMapper.
inline constexpr std::array<const char*, 4> kLookToneMapOperatorNames = {"aces", "filmic", "reinhard", "neutral"};
/// Enum value names for `param` (empty span for non-enum parameters).
u32 look_param_enum_count(LookParam param);
const char* look_param_enum_name(LookParam param, u32 value);
bool look_param_enum_from_name(LookParam param, std::string_view name, u32& out);

/// Flat parameter values (fixed size, trivially copyable).
struct LookParamBlock {
    f32 slots[kLookSlotCount] = {};

    f32 get(LookParam p, u32 c = 0) const { return slots[static_cast<u32>(p) * kLookSlotsPerParam + c]; }
    void set(LookParam p, f32 v, u32 c = 0) { slots[static_cast<u32>(p) * kLookSlotsPerParam + c] = v; }
    math::Vec3 color(LookParam p) const {
        const u32 b = static_cast<u32>(p) * kLookSlotsPerParam;
        return {slots[b], slots[b + 1], slots[b + 2]};
    }
    void setColor(LookParam p, const math::Vec3& v) {
        const u32 b = static_cast<u32>(p) * kLookSlotsPerParam;
        slots[b] = v.x;
        slots[b + 1] = v.y;
        slots[b + 2] = v.z;
    }
};

/// The schema defaults (the neutral look).
const LookParamBlock& look_default_params();

/// Which parameters a profile overrides.
struct LookParamMask {
    static constexpr u32 kWords = (kLookParamCount + 63u) / 64u;
    u64 bits[kWords] = {};
    bool test(LookParam p) const {
        const u32 i = static_cast<u32>(p);
        return (bits[i / 64u] >> (i % 64u)) & 1u;
    }
    void set(LookParam p) {
        const u32 i = static_cast<u32>(p);
        bits[i / 64u] |= u64{1} << (i % 64u);
    }
    bool any() const {
        for (u64 w : bits) {
            if (w != 0u) {
                return true;
            }
        }
        return false;
    }
    u32 count() const;
};

/// Clamps every parameter into its [min, max] range (ints/enums/bools snapped). Returns the number of
/// slots that changed.
u32 look_clamp_params(LookParamBlock& block);

// ---------------------------------------------------------------------------------------------
// Resolved, typed view consumed by the post passes.

enum class LookToneMapOperator : u8 { Aces = 0, Filmic = 1, Reinhard = 2, Neutral = 3 };

struct LookResolved {
    struct {
        bool enabled;
        f32 ssao_strength, ssil_strength;
    } ambient_occlusion;
    struct {
        bool enabled, near_blur;
        f32 focus_distance_m, focal_length_mm, f_stop, max_coc_radius_px;
        u32 bokeh_blades;
    } dof;
    struct {
        bool enabled;
        f32 shutter_angle, max_blur_px;
        u32 max_samples;
    } motion_blur;
    struct {
        bool enabled;
        f32 threshold, knee, intensity, scatter;
        math::Vec3 tint;
    } bloom;
    struct {
        bool enabled;
        f32 intensity;
        math::Vec3 tint;
    } lens_dirt;
    struct {
        bool enabled;
        f32 intensity, threshold, ghost_spacing, halo_radius, halo_thickness, halo_intensity, chromatic_shift;
        u32 ghost_count;
        math::Vec3 tint;
    } lens_flare;
    struct {
        bool auto_enabled;
        f32 bias_ev, min_ev, max_ev, adapt_speed_up, adapt_speed_down;
    } exposure;
    struct {
        LookToneMapOperator op;
        bool calibrate_mid_grey;
    } tonemap;
    struct {
        bool enabled;
        f32 temperature_k, tint, contrast, contrast_pivot, saturation, lut_strength;
        math::Vec3 lift, gamma, gain;
        math::Vec3 curve[5];
    } grade;
    struct {
        bool enabled;
        f32 sharpness;
    } sharpen;
    struct {
        bool enabled;
        f32 intensity;
    } chromatic_aberration;
    struct {
        bool enabled;
        f32 intensity, falloff, roundness;
        math::Vec3 color;
    } vignette;
    struct {
        bool enabled;
        f32 intensity, response;
    } film_grain;
    struct {
        f32 paper_white_nits, peak_nits;
    } output;
};

/// Typed view of a (clamped) parameter block.
LookResolved look_resolve(const LookParamBlock& block);

} // namespace fuse::renderer::look
