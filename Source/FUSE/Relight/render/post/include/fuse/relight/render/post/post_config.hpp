// FUSE Relight RL-5.7: post and upscale wiring - configuration (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.6, §2.8 steps
// 7-9; component table row 31).
//
// The rtx.conf options Remix games ship with pick the upscaler, its quality preset (the render resolution), the texture
// mip bias and the tone mapper; the relight.post.* options are FUSE's switches on top:
//
//   rtx.upscalerType        Remix UpscalerType as FUSE reads it: 0 None (native), 1 DLSS, 2 NIS, 3 TAA-U, 4 XeSS.
//                           DLSS / XeSS only exist through the runtime plugins (plugins/nvidia, plugins/intel_xess):
//                           unregistered -> the in-tree temporal upscaler (native_taau) with the reason recorded.
//   rtx.qualityDLSS         DLSS preset: 0 UltraPerf 0.33, 1 MaxPerf 0.5, 2 Balanced 0.58, 3 MaxQuality 0.667,
//                           4 FullResolution 1.0, 5 Auto (output height <= 1080: MaxQuality, <= 1440: Balanced, else
//                           MaxPerf).
//   rtx.taauPreset          TAA-U preset (also used by fsr1 / fsr3 / XeSS): 0 UltraPerformance 0.33, 1 Performance
//                           0.5, 2 Balanced 0.66, 3 Quality 0.75, 4 Fullscreen 1.0, 5 Custom (rtx.resolutionScale).
//   rtx.nisPreset           NIS preset: 0 Performance 0.5, 1 Balanced 0.66, 2 Quality 0.75, 3 Fullscreen 1.0.
//   rtx.resolutionScale     render / output per axis for the Custom preset.
//   rtx.nativeMipBias       texture LOD bias when not upscaling.
//   rtx.upscalingMipBias    added to log2(render / output) when upscaling (the ported policy, §5.6).
//   rtx.tonemappingMode     0 Global, 1 Local (the ported local tone mapper runs as the Look node before tone mapping).
//   rtx.localtonemap.*      the local tone mapper (local_tonemap.hpp).
//   relight.post.enable     the path tracer's output goes through this pipeline (composite -> upscale -> Look) instead
//                           of RL-5.1's plain "x exposure, sRGB" pack. Off by default until RL-5.5's denoiser lands.
//   relight.post.upscaler   FUSE override of rtx.upscalerType by registry id: none, native_taau, fsr1, fsr3, nis, cas,
//                           dlss, dlss_rr, xess ("" = rtx.upscalerType).
//   relight.post.resolutionScale  > 0 overrides the preset's scale.
//
// Mip bias (§5.6, "the mip bias follows the upscale ratio"): render == output -> rtx.nativeMipBias, otherwise
// log2(render.width / output.width) + rtx.upscalingMipBias (e.g. -1 at 2x). The renderer's temporal guidance adds a
// further -1 (upscale::mip_lod_bias); Remix does not, and this port follows Remix.
#pragma once

#include <fuse/relight/options/option.hpp>
#include <fuse/types.hpp>

#include <string>

namespace fuse::relight::render::post {

struct PostOptions {
    FUSE_RELIGHT_OPTION_ENV("relight.post", bool, enable, false, "FUSE_RELIGHT_POST",
                            "Path tracer output through the RL-5.7 post pipeline (composite, upscale, Look).");
    FUSE_RELIGHT_OPTION_ENV("relight.post", std::string, upscaler, "", "FUSE_RELIGHT_POST_UPSCALER",
                            "Upscaler registry id overriding rtx.upscalerType (none, native_taau, fsr1, fsr3, nis, "
                            "cas, dlss, dlss_rr, xess); empty: rtx.upscalerType.");
    FUSE_RELIGHT_OPTION_ENV("relight.post", float, resolutionScale, 0.f, "FUSE_RELIGHT_POST_RESOLUTION_SCALE",
                            "Render / output scale per axis overriding the upscaler preset (0: the preset).");
    FUSE_RELIGHT_OPTION("relight.post", float, sharpness, 0.2f,
                        "Sharpness of the spatial upscalers / sharpeners in [0, 1] (1 = sharpest).");
    FUSE_RELIGHT_OPTION("relight.post", bool, useAccumulation, true,
                        "Frames that continue the path tracer's accumulation use the accumulated mean instead of "
                        "this frame's composite.");
};

/// The Remix rtx.conf options of the upscaler / tone mapper.
struct RemixPostOptions {
    FUSE_RELIGHT_OPTION("rtx", int, upscalerType, 1,
                        "Upscaler: 0 = None (native), 1 = DLSS, 2 = NIS, 3 = TAA-U, 4 = XeSS. DLSS / XeSS come "
                        "from the runtime plugins; without them the in-tree TAA-U runs.");
    FUSE_RELIGHT_OPTION("rtx", int, qualityDLSS, 5,
                        "DLSS preset: 0 UltraPerf, 1 MaxPerf, 2 Balanced, 3 MaxQuality, 4 FullResolution, 5 Auto.");
    FUSE_RELIGHT_OPTION("rtx", int, taauPreset, 2,
                        "TAA-U preset: 0 UltraPerformance, 1 Performance, 2 Balanced, 3 Quality, 4 Fullscreen, "
                        "5 Custom (rtx.resolutionScale).");
    FUSE_RELIGHT_OPTION("rtx", int, nisPreset, 1, "NIS preset: 0 Performance, 1 Balanced, 2 Quality, 3 Fullscreen.");
    FUSE_RELIGHT_OPTION("rtx", float, resolutionScale, 0.75f,
                        "Render / output scale per axis of the Custom TAA-U preset.");
    FUSE_RELIGHT_OPTION("rtx", float, nativeMipBias, 0.f, "Texture LOD bias when rendering at the output resolution.");
    FUSE_RELIGHT_OPTION("rtx", float, upscalingMipBias, 0.f,
                        "Texture LOD bias added to log2(render / output) when upscaling.");
    FUSE_RELIGHT_OPTION("rtx", int, tonemappingMode, 1, "Tone mapping: 0 = Global, 1 = Local.");
};

struct LocalToneMapOptions {
    FUSE_RELIGHT_OPTION("rtx.localtonemap", int, mip, 3, "Pyramid levels below full resolution of the exposure fusion.");
    FUSE_RELIGHT_OPTION("rtx.localtonemap", float, shadows, 2.f, "Brightening factor of the over-exposed candidate.");
    FUSE_RELIGHT_OPTION("rtx.localtonemap", float, highlights, 4.f,
                        "Darkening factor of the under-exposed candidate.");
    FUSE_RELIGHT_OPTION("rtx.localtonemap", float, exposurePreferenceSigma, 4.f,
                        "Sharpness of the well-exposedness weight around mid grey.");
    FUSE_RELIGHT_OPTION("rtx.localtonemap", float, exposurePreferenceOffset, 0.f,
                        "Offset of the preferred (perceptual) exposure from 0.5.");
};

/// Registers the options above (call once; keeps them linked in).
void registerPostOptions();

enum class ToneMappingMode : u8 { Global = 0, Local = 1 };

struct LocalToneMapConfig {
    u32 mip = 3;
    float shadows = 2.f;
    float highlights = 4.f;
    float exposurePreferenceSigma = 4.f;
    float exposurePreferenceOffset = 0.f;
};

struct PostConfig {
    std::string upscaler = "native_taau"; ///< requested registry id ("none": native)
    float resolutionScale = 1.f;          ///< render / output per axis for the requested upscaler
    float nativeMipBias = 0.f;
    float upscalingMipBias = 0.f;
    float sharpness = 0.2f;
    bool useAccumulation = true;
    ToneMappingMode toneMapping = ToneMappingMode::Local;
    LocalToneMapConfig localToneMap{};

    /// From the options above; `outputHeight` resolves rtx.qualityDLSS = Auto.
    static PostConfig fromOptions(u32 outputHeight);
};

/// Registry id for a Remix rtx.upscalerType value (unknown -> "native_taau").
const char* upscalerNameForRemixType(int type);
/// Preset scales (see the header comment); out-of-range presets clamp to the nearest one.
float dlssPresetScale(int preset, u32 outputHeight);
float taauPresetScale(int preset, float customScale);
float nisPresetScale(int preset);

/// Render extent for `scale` (per axis round(output * scale), at least 1, at most the output).
void renderExtentForScale(u32 outputWidth, u32 outputHeight, float scale, u32& width, u32& height);

/// The texture LOD bias of a frame rendered at `renderWidth` and shown at `outputWidth` (see the header comment).
float textureMipBias(u32 renderWidth, u32 outputWidth, float nativeMipBias, float upscalingMipBias);

} // namespace fuse::relight::render::post
