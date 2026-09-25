#pragma once

// Host entry points for the single-source spatial upscale / sharpen kernels (fsr1_kernel.hpp,
// cas_kernel.hpp, nis_kernel.hpp, bilinear baseline). Each pass computes its constants exactly the way the
// vendored SDK's host helper does (bit patterns, so a GPU pass fed the same constants runs the same math)
// and launches the kernel through kernel::launch (CpuReference / CpuParallel today; Cuda / VulkanCompute
// fall back to CpuParallel and the fallback is recorded in the kernel stats).
//
// These are usable on their own in the post chain: run_cas() / run_rcas() are same-resolution sharpeners.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/upscale/cas_kernel.hpp>
#include <fuse/renderer/upscale/fsr1_kernel.hpp>
#include <fuse/renderer/upscale/nis_kernel.hpp>
#include <fuse/renderer/upscale/upscaler.hpp>

namespace fuse::renderer::upscale {

/// ffxFsrPopulateEasuConstants (FidelityFX SDK v1.1.4): input viewport = input image size, no offset.
kernels::EasuConstants easu_constants(u32 input_width, u32 input_height, u32 output_width, u32 output_height);
/// ffxFsrPopulateEasuConstants with a viewport inside a larger input resource (dynamic resolution).
kernels::EasuConstants easu_constants_viewport(f32 viewport_width, f32 viewport_height, u32 input_width,
                                               u32 input_height, u32 output_width, u32 output_height);
/// FsrRcasCon: `sharpness_stops` 0 = maximum sharpness, each +1 halves it.
kernels::RcasConstants rcas_constants(f32 sharpness_stops);
/// FUSE's [0, 1] sharpness (1 = sharpest) to RCAS stops: stops = 2 * (1 - sharpness) (0.2 -> 1.6 stops).
f32 rcas_stops_from_sharpness(f32 sharpness);
/// ffxCasSetup (`sharpness` in [0, 1]); input == output size for the sharpen-only path.
kernels::CasConstants cas_constants(f32 sharpness, u32 input_width, u32 input_height, u32 output_width,
                                    u32 output_height);
/// NVScalerUpdateConfig (vendored NIS_Config.h, SDR). False when the ratio is outside NVScaler's 1..2x.
bool nis_constants(f32 sharpness, u32 input_width, u32 input_height, u32 output_width, u32 output_height,
                   kernels::NisConstants& out);
/// NIS_Config.h coef_scale / coef_usm (64 phases x 8 floats).
std::span<const f32> nis_coef_scale();
std::span<const f32> nis_coef_usm();

struct PassOptions {
    kernel::Backend backend = kernel::Backend::CpuParallel;
    kernel::LaunchOptions launch{};
};

/// EASU `src` -> `dst` (any size; dst is the display image).
bool run_easu(const ConstRgbaImage& src, const RgbaImage& dst, const PassOptions& options = {});
/// RCAS `src` -> `dst` (same size, must not alias).
bool run_rcas(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness_stops, const PassOptions& options = {},
              bool denoise = true, bool passthrough_alpha = false);
/// CAS sharpen-only `src` -> `dst` (same size, must not alias).
bool run_cas(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness, const PassOptions& options = {},
             bool better_diagonals = false, bool precise_math = false);
/// NVScaler `src` -> `dst` (dst 1..2x src per axis).
bool run_nis(const ConstRgbaImage& src, const RgbaImage& dst, f32 sharpness, const PassOptions& options = {});
/// Bilinear baseline `src` -> `dst`.
bool run_bilinear(const ConstRgbaImage& src, const RgbaImage& dst, const PassOptions& options = {});

} // namespace fuse::renderer::upscale
