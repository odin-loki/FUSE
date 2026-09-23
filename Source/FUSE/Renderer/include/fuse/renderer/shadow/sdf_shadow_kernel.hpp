#pragma once

// Single-source SDF soft-shadow pass (docs/compute-kernels.md): one pixel per item, 8x8 tiles. The
// occluder SDF is compute::ray_march_kernel::scene_eval (the ray march's scene, not a copy) and the
// penumbra march is sdfSoftShadow (shadow/sdf_soft_shadow.hpp) — both FUSE_HOST_DEVICE, so the CPU
// backends (src/shadow/sdf_shadows.cpp) and the CUDA wrapper (kernels/sdf_shadows.cu) run the same code.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/renderer/shadow/sdf_shadows.hpp>
#include <fuse/renderer/shadow/sdf_soft_shadow.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::renderer::sdf_shadow_kernel {

/// Kernel / profiler / GPU-timestamp name (matches DeferredFramePipeline's SdfShadows pass).
inline constexpr const char* kName = "sdf_shadows";
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};

/// The ray-march scene as the SceneSdf functor sdfSoftShadow expects.
struct SceneSdf {
    const compute::RayMarchParams* scene = nullptr;
    FUSE_HOST_DEVICE f32 operator()(const math::Vec3& p) const {
        return compute::ray_march_kernel::scene_eval(*scene, p, nullptr);
    }
};

struct Params {
    compute::RayMarchParams scene{}; ///< objects / object_count / max_dist are used.
    u32 width = 0;
    u32 height = 0;
    const math::Vec4* positions = nullptr;
    const math::Vec3* normals = nullptr;
    math::Vec3 light_dir{0.f, 1.f, 0.f}; ///< unit, toward the light
    f32 normal_bias = 0.f;
    SdfSoftShadowParams march{};
    f32* out_shadow = nullptr;
};

inline bool params_valid(const SdfShadowFrame& f) {
    return f.width != 0u && f.height != 0u && f.worldPositions != nullptr && f.outShadow != nullptr &&
           (f.objectCount == 0u || f.objects != nullptr) && f.lightDirection.dot(f.lightDirection) > 1e-12f &&
           f.march.tMax > f.march.tMin && f.march.tMin >= 0.f && f.sceneMaxDistance > 0.f;
}

inline Params make_params(const SdfShadowFrame& f) {
    Params p{};
    p.scene.objects = f.objects;
    p.scene.object_count = f.objectCount;
    p.scene.max_dist = f.sceneMaxDistance;
    p.width = f.width;
    p.height = f.height;
    p.positions = f.worldPositions;
    p.normals = f.normals;
    p.light_dir = f.lightDirection.normalized();
    p.normal_bias = f.normalBias;
    p.march = f.march;
    p.out_shadow = f.outShadow;
    return p;
}

inline kernel::KernelLaunch make_launch(const SdfShadowFrame& f) {
    return kernel::KernelLaunch{kName, kernel::extent2(f.width, f.height), kWorkgroup};
}

/// Shadow factor of one surface point (the public scalar path and the kernel share it).
FUSE_HOST_DEVICE inline f32 shadow_at(const Params& p, const math::Vec3& position, const math::Vec3* normal) {
    const math::Vec3 origin = normal != nullptr ? position + *normal * p.normal_bias : position;
    return sdfSoftShadow(SceneSdf{&p.scene}, origin, p.light_dir, p.march);
}

struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const u32 pixel = idx.global.y * p.width + idx.global.x;
        const math::Vec4 surface = p.positions[pixel];
        if (!(surface.w > 0.f)) {
            p.out_shadow[pixel] = 1.f;
            return;
        }
        p.out_shadow[pixel] = shadow_at(p, math::Vec3{surface.x, surface.y, surface.z},
                                        p.normals != nullptr ? &p.normals[pixel] : nullptr);
    }
};

} // namespace fuse::renderer::sdf_shadow_kernel
