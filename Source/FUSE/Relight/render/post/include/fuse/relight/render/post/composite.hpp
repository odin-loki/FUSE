// FUSE Relight RL-5.7: demodulate / composite (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8 step 7, §5.5, component row 31:
// Remix's rtx_demodulate / rtx_composite).
//
// The path tracer writes its radiance demodulated (kernels/pt_reference_core.h, DEMODULATION): radiance = emissive +
// diffuse x albedoD + specular x albedoS, with diffuse / specular divided by the G-buffer albedos (so the denoiser
// filters illumination, not texture). The composite remodulates the (denoised, when RL-5.5's denoiser runs) channels:
//
//   out = emissive + diffuse' x albedoD + specular' x albedoS       diffuse' / specular' = denoised or raw channel
//
// With the denoiser off it reproduces the path tracer's own radiance (exact up to the float round trip of the division
// by the albedo: a relative 2^-22 per channel; gate rl_post_composite on the CPU reference path tracer's channels).
// Inputs are the PathTracerGpu output sections (f32x4 per pixel, pt_gpu.hpp kPtOut*), so the pass reads the path
// tracer's buffer in place.
#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <cstddef>

namespace fuse::relight::render::post {

struct CompositeInputs {
    const float* emissive = nullptr; ///< f32x4 (rgb, -)
    const float* diffuse = nullptr;  ///< f32x4 demodulated diffuse radiance (rgb, hit distance)
    const float* specular = nullptr; ///< f32x4 demodulated specular radiance (rgb, hit distance)
    const float* albedoD = nullptr;  ///< f32x4 (rgb, PSR chain length)
    const float* albedoS = nullptr;  ///< f32x4 (rgb, flags)
    const float* denoisedDiffuse = nullptr;  ///< optional f32x4 (RL-5.5 denoiser output); null: `diffuse`
    const float* denoisedSpecular = nullptr; ///< optional f32x4; null: `specular`
    std::size_t count = 0;
};

/// Remodulates `in` into `out` (count pixels, linear scene radiance). False on missing inputs.
bool compositeRadiance(const CompositeInputs& in, math::Vec3* out);

} // namespace fuse::relight::render::post
