// Host side of the single-source spatial upscale / sharpen kernels: SDK-equivalent constant setup and
// kernel::launch wrappers (see upscale_passes.hpp).

#include <fuse/renderer/upscale/upscale_passes.hpp>

// Vendored NVIDIA Image Scaling SDK v1.0.3 (Engine/lib/nvidia-nis, MIT): NVScalerUpdateConfig and the
// coef_scale / coef_usm filter banks. Included as a SYSTEM header (third-party warning levels).
#include <NIS_Config.h>

#include <algorithm>
#include <cmath>

namespace fuse::renderer::upscale {

namespace {

using kernels::as_u32;

bool same_size(const ConstRgbaImage& a, const RgbaImage& b) { return a.width == b.width && a.height == b.height; }

bool no_alias(const ConstRgbaImage& a, const RgbaImage& b) {
    const math::Vec4* aBegin = a.data;
    const math::Vec4* aEnd = a.data + static_cast<usize>(a.width) * a.height;
    const math::Vec4* bBegin = b.data;
    const math::Vec4* bEnd = b.data + static_cast<usize>(b.width) * b.height;
    return aEnd <= bBegin || bEnd <= aBegin;
}

kernels::RgbaSurface surface(const ConstRgbaImage& image) { return {image.data, image.width, image.height}; }
kernels::RgbaTarget target(const RgbaImage& image) { return {image.data, image.width, image.height}; }

/// ffxLerp in ffx_core_cpu.h.
f32 ffx_lerp(f32 x, f32 y, f32 t) { return y * t + (-x * t + x); }

f32 saturate_host(f32 x) { return std::min(1.f, std::max(0.f, x)); }

template <typename Kernel, typename Params>
bool launch_pass(const char* name, u32 width, u32 height, const Params& params, const PassOptions& options) {
    return kernel::launch(options.backend, kernels::pixel_launch(name, width, height), Kernel{}, params, options.launch)
        .ok;
}

} // namespace

kernels::EasuConstants easu_constants_viewport(f32 viewport_width, f32 viewport_height, u32 input_width,
                                               u32 input_height, u32 output_width, u32 output_height) {
    // Transcription of ffxFsrPopulateEasuConstants (ffx_fsr1.h): same float operations, same order.
    const f32 inW = static_cast<f32>(input_width);
    const f32 inH = static_cast<f32>(input_height);
    const f32 outW = static_cast<f32>(output_width);
    const f32 outH = static_cast<f32>(output_height);
    kernels::EasuConstants c{};
    // Output integer position to a pixel position in viewport.
    c.con0[0] = as_u32(viewport_width * (1.0f / outW));
    c.con0[1] = as_u32(viewport_height * (1.0f / outH));
    c.con0[2] = as_u32(0.5f * viewport_width * (1.0f / outW) - 0.5f);
    c.con0[3] = as_u32(0.5f * viewport_height * (1.0f / outH) - 0.5f);
    // Viewport pixel position to normalized image space (upper-left of the 'F' tap).
    c.con1[0] = as_u32(1.0f / inW);
    c.con1[1] = as_u32(1.0f / inH);
    // Centers of gather4, first offset from upper-left of 'F'.
    c.con1[2] = as_u32(1.0f * (1.0f / inW));
    c.con1[3] = as_u32(-1.0f * (1.0f / inH));
    // These are from (0) instead of 'F'.
    c.con2[0] = as_u32(-1.0f * (1.0f / inW));
    c.con2[1] = as_u32(2.0f * (1.0f / inH));
    c.con2[2] = as_u32(1.0f * (1.0f / inW));
    c.con2[3] = as_u32(2.0f * (1.0f / inH));
    c.con3[0] = as_u32(0.0f * (1.0f / inW));
    c.con3[1] = as_u32(4.0f * (1.0f / inH));
    c.con3[2] = 0u;
    c.con3[3] = 0u;
    return c;
}

kernels::EasuConstants easu_constants(u32 input_width, u32 input_height, u32 output_width, u32 output_height) {
    return easu_constants_viewport(static_cast<f32>(input_width), static_cast<f32>(input_height), input_width,
                                   input_height, output_width, output_height);
}

kernels::RcasConstants rcas_constants(f32 sharpness_stops) {
    // FsrRcasCon: transform from stops to linear value. con[1] (packed half2 for the FP16 path) is left 0:
    // FUSE runs the float32 path only.
    const f32 sharpness = std::exp2(-sharpness_stops);
    kernels::RcasConstants c{};
    c.con[0] = as_u32(sharpness);
    return c;
}

f32 rcas_stops_from_sharpness(f32 sharpness) { return 2.f * (1.f - saturate_host(sharpness)); }

kernels::CasConstants cas_constants(f32 sharpness, u32 input_width, u32 input_height, u32 output_width,
                                    u32 output_height) {
    // Transcription of ffxCasSetup (ffx_cas.h). const1[1] (packed half2 for the FP16 path) is left 0.
    const f32 inW = static_cast<f32>(input_width);
    const f32 inH = static_cast<f32>(input_height);
    const f32 outW = static_cast<f32>(output_width);
    const f32 outH = static_cast<f32>(output_height);
    kernels::CasConstants c{};
    c.const0[0] = as_u32(inW * (1.0f / outW));
    c.const0[1] = as_u32(inH * (1.0f / outH));
    c.const0[2] = as_u32(0.5f * inW * (1.0f / outW) - 0.5f);
    c.const0[3] = as_u32(0.5f * inH * (1.0f / outH) - 0.5f);
    const f32 sharp = -(1.0f / ffx_lerp(8.0f, 5.0f, saturate_host(sharpness)));
    c.const1[0] = as_u32(sharp);
    c.const1[1] = 0u;
    c.const1[2] = as_u32(8.0f * inW * (1.0f / outW));
    c.const1[3] = 0u;
    return c;
}

bool nis_constants(f32 sharpness, u32 input_width, u32 input_height, u32 output_width, u32 output_height,
                   kernels::NisConstants& out) {
    NISConfig config{};
    if (!NVScalerUpdateConfig(config, sharpness, 0u, 0u, input_width, input_height, input_width, input_height, 0u,
                              0u, output_width, output_height, output_width, output_height, NISHDRMode::None)) {
        return false;
    }
    out.detect_ratio = config.kDetectRatio;
    out.detect_thres = config.kDetectThres;
    out.min_contrast_ratio = config.kMinContrastRatio;
    out.ratio_norm = config.kRatioNorm;
    out.contrast_boost = config.kContrastBoost;
    out.eps = config.kEps;
    out.sharp_start_y = config.kSharpStartY;
    out.sharp_scale_y = config.kSharpScaleY;
    out.sharp_strength_min = config.kSharpStrengthMin;
    out.sharp_strength_scale = config.kSharpStrengthScale;
    out.sharp_limit_min = config.kSharpLimitMin;
    out.sharp_limit_scale = config.kSharpLimitScale;
    out.scale_x = config.kScaleX;
    out.scale_y = config.kScaleY;
    return true;
}

static_assert(sizeof(coef_scale) == sizeof(f32) * kernels::kNisCoefCount, "NIS coefficient bank layout");
static_assert(sizeof(coef_usm) == sizeof(f32) * kernels::kNisCoefCount, "NIS coefficient bank layout");

std::span<const f32> nis_coef_scale() { return {&coef_scale[0][0], kernels::kNisCoefCount}; }
std::span<const f32> nis_coef_usm() { return {&coef_usm[0][0], kernels::kNisCoefCount}; }

bool run_easu(const ConstRgbaImage& src, const RgbaImage& dst, const PassOptions& options) {
    if (!src.valid() || !dst.valid() || !no_alias(src, dst)) {
        return false;
    }
    const kernels::EasuParams params{surface(src), target(dst), easu_constants(src.width, src.height, dst.width, dst.height)};
    return launch_pass<kernels::EasuKernel>(kernels::kEasuKernelName, dst.width, dst.height, params, options);
}

bool run_rcas(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness_stops, const PassOptions& options,
              bool denoise, bool passthrough_alpha) {
    if (!src.valid() || !dst.valid() || !same_size(src, dst) || !no_alias(src, dst)) {
        return false;
    }
    const kernels::RcasParams params{surface(src), target(dst), rcas_constants(sharpness_stops), denoise ? 1u : 0u,
                                     passthrough_alpha ? 1u : 0u};
    return launch_pass<kernels::RcasKernel>(kernels::kRcasKernelName, dst.width, dst.height, params, options);
}

bool run_cas(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness, const PassOptions& options,
             bool better_diagonals, bool precise_math) {
    if (!src.valid() || !dst.valid() || !same_size(src, dst) || !no_alias(src, dst)) {
        return false;
    }
    const kernels::CasParams params{surface(src), target(dst),
                                    cas_constants(sharpness, src.width, src.height, dst.width, dst.height),
                                    better_diagonals ? 1u : 0u, precise_math ? 1u : 0u};
    return launch_pass<kernels::CasKernel>(kernels::kCasKernelName, dst.width, dst.height, params, options);
}

bool run_nis(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness, const PassOptions& options) {
    if (!src.valid() || !dst.valid() || !no_alias(src, dst)) {
        return false;
    }
    kernels::NisParams params{};
    if (!nis_constants(sharpness, src.width, src.height, dst.width, dst.height, params.k)) {
        return false;
    }
    params.src = surface(src);
    params.dst = target(dst);
    params.coef_scale = nis_coef_scale().data();
    params.coef_usm = nis_coef_usm().data();
    return launch_pass<kernels::NisKernel>(kernels::kNisKernelName, dst.width, dst.height, params, options);
}

bool run_bilinear(const ConstRgbaImage& src, const RgbaImage& dst, const PassOptions& options) {
    if (!src.valid() || !dst.valid() || !no_alias(src, dst)) {
        return false;
    }
    const kernels::BilinearParams params{surface(src), target(dst)};
    return launch_pass<kernels::BilinearKernel>(kernels::kBilinearKernelName, dst.width, dst.height, params, options);
}

} // namespace fuse::renderer::upscale
