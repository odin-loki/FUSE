#pragma once

// Single-source SDF ray march (the reference port of the compute kernel model — docs/compute-kernels.md).
// Every function here is FUSE_HOST_DEVICE and is the ONLY implementation of the scene SDF, its
// analytic gradient and the sphere trace: the CPU reference / parallel backends (ray_march_cpu.cpp)
// and the CUDA trampoline (kernels/ray_march.cu) all run this code.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/sdf.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::compute::ray_march_kernel {

/// Kernel / profiler / GPU-timestamp name (matches DeferredFramePipeline's SdfRayMarch pass).
inline constexpr const char* kName = "sdf_ray_march";
/// 8x8 tiles: coherent rays per workgroup (CUDA block / Vulkan local_size 8x8).
inline constexpr kernel::Dim3 kWorkgroup{8u, 8u, 1u};

FUSE_HOST_DEVICE inline f32 clamped_rounding(const SdfObject& obj) {
    const f32 smallest = std::min(obj.params.x, std::min(obj.params.y, obj.params.z));
    return std::clamp(obj.rounding, 0.f, std::max(smallest, 0.f));
}

/// Distance of one primitive in its local frame; writes the analytic gradient to `*grad` when non-null.
FUSE_HOST_DEVICE inline f32 object_sdf(const SdfObject& obj, math::Vec3 local, f32 fallback, math::Vec3* grad) {
    switch (static_cast<SdfPrimitiveType>(obj.type)) {
    case SdfPrimitiveType::Sphere:
        if (grad != nullptr) {
            *grad = math::SDF::sphereGradient(local);
        }
        return math::SDF::sphere(local, obj.params.x);
    case SdfPrimitiveType::Box: {
        const f32 r = clamped_rounding(obj);
        const math::Vec3 inner{obj.params.x - r, obj.params.y - r, obj.params.z - r};
        if (grad != nullptr) {
            *grad = math::SDF::boxGradient(local, inner);
        }
        return math::SDF::box(local, inner) - r;
    }
    case SdfPrimitiveType::Capsule: {
        const f32 h = std::max(obj.params.y, 0.f);
        const math::Vec3 v{local.x, local.y - std::clamp(local.y, -h, h), local.z};
        if (grad != nullptr) {
            *grad = math::SDF::sphereGradient(v);
        }
        return v.length() - obj.params.x;
    }
    case SdfPrimitiveType::Torus: {
        const f32 radial = std::sqrt(local.x * local.x + local.z * local.z);
        const f32 ring = radial - obj.params.x;
        const f32 tube = std::sqrt(ring * ring + local.y * local.y);
        if (grad != nullptr) {
            if (tube < 1e-8f) {
                *grad = {1.f, 0.f, 0.f};
            } else {
                const f32 rx = radial > 1e-8f ? local.x / radial : 1.f;
                const f32 rz = radial > 1e-8f ? local.z / radial : 0.f;
                const f32 s = ring / tube;
                *grad = {s * rx, local.y / tube, s * rz};
            }
        }
        return tube - obj.params.y;
    }
    default:
        if (grad != nullptr) {
            *grad = {};
        }
        return fallback;
    }
}

/// Scene distance (hard / smooth union, in object order) and, when `grad` is non-null, its analytic gradient.
/// Smooth union d = min(a, b) - h^2 k / 4 with h = max(k - |a - b|, 0) / k has, for a <= b,
/// grad d = (1 - h/2) grad a + (h/2) grad b (continuous across a = b where both weights are 1/2).
FUSE_HOST_DEVICE inline f32 scene_eval(const RayMarchParams& params, math::Vec3 position, math::Vec3* grad) {
    f32 distance = params.max_dist;
    math::Vec3 gradient{};

    for (u32 i = 0; i < params.object_count; ++i) {
        const SdfObject& obj = params.objects[i];
        math::Vec3 objectGrad{};
        const f32 objectDistance =
            object_sdf(obj, position - obj.position, params.max_dist, grad != nullptr ? &objectGrad : nullptr);
        // A zero blend width would divide by zero in opSmoothUnion — treat it as a hard union.
        if (obj.alpha > 0.f) {
            if (grad != nullptr) {
                const f32 h = std::max(obj.alpha - std::abs(distance - objectDistance), 0.f) / obj.alpha;
                const f32 wMin = 1.f - 0.5f * h;
                const f32 wMax = 0.5f * h;
                gradient = distance <= objectDistance ? gradient * wMin + objectGrad * wMax
                                                      : gradient * wMax + objectGrad * wMin;
            }
            distance = math::SDF::opSmoothUnion(distance, objectDistance, obj.alpha);
        } else {
            if (objectDistance < distance) {
                gradient = objectGrad;
            }
            distance = std::min(distance, objectDistance);
        }
    }

    if (grad != nullptr) {
        *grad = gradient;
    }
    return distance;
}

FUSE_HOST_DEVICE inline math::Vec3 scene_normal(const RayMarchParams& params, math::Vec3 position) {
    math::Vec3 gradient{};
    (void)scene_eval(params, position, &gradient);
    const f32 len = gradient.length();
    if (!(len > 1e-12f)) {
        return {0.f, 1.f, 0.f};
    }
    return gradient * (1.f / len);
}

/// Sphere trace of one ray (`direction` need not be unit); hit distance or -1.
FUSE_HOST_DEVICE inline f32 hit_distance(const RayMarchParams& params, math::Vec3 origin, math::Vec3 direction) {
    const math::Vec3 rayDirection = direction.normalized();
    if (rayDirection.length() <= 0.f) {
        return -1.f;
    }
    f32 t = 0.f;
    for (u32 step = 0; step < params.max_steps && t < params.max_dist; ++step) {
        const f32 distance = scene_eval(params, origin + rayDirection * t, nullptr);
        if (distance < params.min_dist) {
            return t;
        }
        t += distance;
    }
    return -1.f;
}

/// Full-frame launch params: the scene + surfaces plus the camera basis resolved once on the host.
struct Params {
    RayMarchParams scene{};
    math::Vec3 forward{};
    math::Vec3 right{};
    math::Vec3 up{};
    f32 tan_half = 0.f;
    f32 aspect = 1.f;
};

/// Host-side validation shared by every backend (false = reject the launch).
inline bool params_valid(const RayMarchParams& p) {
    return p.width != 0 && p.height != 0 && p.max_steps != 0 && p.min_dist > 0.f && p.max_dist > p.min_dist &&
           p.fov_rad > 0.f && p.fov_rad < 3.14159265f && (p.object_count == 0 || p.objects != nullptr);
}

inline Params make_params(const RayMarchParams& scene) {
    Params p{};
    p.scene = scene;
    p.forward = scene.cam_forward.normalized();
    p.right = scene.cam_right.normalized();
    p.up = scene.cam_up.normalized();
    p.tan_half = std::tan(0.5f * scene.fov_rad);
    p.aspect = static_cast<f32>(scene.width) / static_cast<f32>(scene.height);
    return p;
}

inline kernel::KernelLaunch make_launch(const RayMarchParams& scene) {
    return kernel::KernelLaunch{kName, kernel::extent2(scene.width, scene.height), kWorkgroup};
}

/// The kernel body: one pixel ray through the pixel centre (row 0 = top) -> depth + world normal.
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        const RayMarchParams& s = p.scene;
        const u32 x = idx.global.x;
        const u32 y = idx.global.y;
        const f32 ndcX = (2.f * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(s.width) - 1.f);
        const f32 ndcY = (1.f - 2.f * (static_cast<f32>(y) + 0.5f) / static_cast<f32>(s.height));
        const math::Vec3 direction =
            (p.forward + p.right * (ndcX * p.tan_half * p.aspect) + p.up * (ndcY * p.tan_half)).normalized();
        const f32 t = hit_distance(s, s.cam_pos, direction);
        const u32 pixel = y * s.width + x;
        if (s.depth_surface != nullptr) {
            static_cast<f32*>(s.depth_surface)[pixel] = t;
        }
        if (s.output_surface != nullptr) {
            static_cast<math::Vec4*>(s.output_surface)[pixel] =
                t >= 0.f ? math::Vec4{scene_normal(s, s.cam_pos + direction * t), 1.f} : math::Vec4{};
        }
    }
};

} // namespace fuse::compute::ray_march_kernel
