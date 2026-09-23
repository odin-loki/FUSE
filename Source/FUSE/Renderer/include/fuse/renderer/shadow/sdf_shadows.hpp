#pragma once

// Full-frame SDF soft shadows (B5.5): the `sdf_shadows` render-graph pass as a single-source kernel
// (fuse/renderer/shadow/sdf_shadow_kernel.hpp). Every surface pixel marches a penumbra ray toward a
// directional light through the analytic SDF scene of the ray march (fuse::compute::SdfObject, evaluated
// by compute::ray_march_kernel::scene_eval — one scene SDF for primary rays and shadow rays).

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/shadow/sdf_soft_shadow.hpp>
#include <fuse/types.hpp>

namespace fuse::renderer {

/// Inputs / output of one shadow pass. Arrays are row-major `width * height` host arrays (row 0 = top).
struct SdfShadowFrame {
    u32 width = 0;
    u32 height = 0;
    /// World-space surface position; w > 0 marks a surface pixel, w <= 0 background (shadow = 1).
    const fuse::math::Vec4* worldPositions = nullptr;
    /// Optional world normals: the ray origin is offset by `normalBias` along them (null = no offset).
    const fuse::math::Vec3* normals = nullptr;
    /// Occluders (smooth / hard union in order, as in the ray march).
    const compute::SdfObject* objects = nullptr;
    u32 objectCount = 0;
    /// Scene distance with no objects (upper bound of scene_eval, RayMarchParams::max_dist).
    f32 sceneMaxDistance = 1000.f;
    /// Direction toward the (directional) light; normalized by the launch.
    fuse::math::Vec3 lightDirection{0.f, 1.f, 0.f};
    f32 normalBias = 0.f;
    SdfSoftShadowParams march{};
    /// Output shadow factor in [0, 1] (1 = fully lit).
    f32* outShadow = nullptr;
};

/// Host-side validation shared by every backend (false = the launch is rejected).
bool sdfShadowFrameValid(const SdfShadowFrame& frame);

/// Runs the `sdf_shadows` kernel on `backend` (Auto / Cuda use the device in FUSE_HAS_CUDA builds when
/// one is present; otherwise kernel::launch falls back to CpuParallel and records it). False on invalid
/// input or a failed launch.
bool renderSdfShadows(const SdfShadowFrame& frame, kernel::Backend backend = kernel::Backend::CpuParallel,
                      void* stream = nullptr);

} // namespace fuse::renderer
