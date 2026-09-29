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
#include <bit>
#include <cmath>

namespace fuse::compute::ray_march_kernel {

/// Kernel / profiler / GPU-timestamp name (matches DeferredFramePipeline's SdfRayMarch pass).
inline constexpr const char* kName = "sdf_ray_march";
/// 8x8 tiles: coherent rays per workgroup (CUDA block / Vulkan local_size 8x8).
// 128 threads/block. A 64-thread block tops out at 16 resident blocks on sm_86 (32 of 48
// warps, 66.7% theoretical) and the 1080p march measured 56% achieved. 128 threads clears 60%.
inline constexpr kernel::Dim3 kWorkgroup{16u, 8u, 1u};

FUSE_HOST_DEVICE inline f32 clamped_rounding(const SdfObject& obj) {
    const f32 smallest = std::min(obj.params.x, std::min(obj.params.y, obj.params.z));
    const f32 upper = smallest > 0.f ? smallest : 0.f;
    // std::clamp(rounding, 0, upper) without its reference arguments: nvcc otherwise materialises the
    // bounds in local memory and loads the result through a pointer.
    const f32 r = obj.rounding < 0.f ? 0.f : obj.rounding;
    return upper < r ? upper : r;
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

/// Object sources for the scene fold: every object, or a tile's culled list (ascending object indices,
/// so the fold order — which smooth unions depend on — is the scene's object order either way).
struct AllObjects {
    u32 count = 0;
    FUSE_HOST_DEVICE u32 operator[](u32 i) const { return i; }
};

struct ObjectList {
    const u32* index = nullptr; ///< null: every object (identity), so one march serves both cases
    u32 count = 0;
    FUSE_HOST_DEVICE u32 operator[](u32 i) const { return index != nullptr ? index[i] : i; }
};

/// Scene distance (hard / smooth union, in object order) and, when `grad` is non-null, its analytic gradient.
/// Smooth union d = min(a, b) - h^2 k / 4 with h = max(k - |a - b|, 0) / k has, for a <= b,
/// grad d = (1 - h/2) grad a + (h/2) grad b (continuous across a = b where both weights are 1/2).
/// `objects` selects which objects are folded (AllObjects for the whole scene).
template <typename Objects>
FUSE_HOST_DEVICE inline f32 scene_eval_in(const RayMarchParams& params, Objects objects, math::Vec3 position,
                                          math::Vec3* grad) {
    f32 distance = params.max_dist;
    math::Vec3 gradient{};

    for (u32 k = 0; k < objects.count; ++k) {
        const SdfObject& obj = params.objects[objects[k]];
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

/// Scene distance over every object (the reference fold; see scene_eval_in).
FUSE_HOST_DEVICE inline f32 scene_eval(const RayMarchParams& params, math::Vec3 position, math::Vec3* grad) {
    return scene_eval_in(params, AllObjects{params.object_count}, position, grad);
}

template <typename Objects>
FUSE_HOST_DEVICE inline math::Vec3 scene_normal_in(const RayMarchParams& params, Objects objects,
                                                   math::Vec3 position) {
    math::Vec3 gradient{};
    (void)scene_eval_in(params, objects, position, &gradient);
    const f32 len = gradient.length();
    if (!(len > 1e-12f)) {
        return {0.f, 1.f, 0.f};
    }
    return gradient * (1.f / len);
}

FUSE_HOST_DEVICE inline math::Vec3 scene_normal(const RayMarchParams& params, math::Vec3 position) {
    return scene_normal_in(params, AllObjects{params.object_count}, position);
}

/// Sphere trace of one ray (`direction` need not be unit) through `objects`; hit distance or -1.
template <typename Objects>
FUSE_HOST_DEVICE inline f32 hit_distance_in(const RayMarchParams& params, Objects objects, math::Vec3 origin,
                                            math::Vec3 direction) {
    const math::Vec3 rayDirection = direction.normalized();
    if (rayDirection.length() <= 0.f) {
        return -1.f;
    }
    f32 t = 0.f;
    for (u32 step = 0; step < params.max_steps && t < params.max_dist; ++step) {
        const f32 distance = scene_eval_in(params, objects, origin + rayDirection * t, nullptr);
        if (distance < params.min_dist) {
            return t;
        }
        t += distance;
    }
    return -1.f;
}

/// Sphere trace of one ray (`direction` need not be unit) against the whole scene; hit distance or -1.
FUSE_HOST_DEVICE inline f32 hit_distance(const RayMarchParams& params, math::Vec3 origin, math::Vec3 direction) {
    return hit_distance_in(params, AllObjects{params.object_count}, origin, direction);
}

/// Full-frame launch params: the scene + surfaces plus the camera basis resolved once on the host.
struct Params {
    RayMarchParams scene{};
    math::Vec3 forward{};
    math::Vec3 right{};
    math::Vec3 up{};
    f32 tan_half = 0.f;
    f32 aspect = 1.f;
    /// Tiled march only: every object's bounding sphere is grown by this much before the tile test —
    /// twice the widest smooth-union blend in the scene, so objects whose blend can reach a visible
    /// surface in the tile stay in its list (make_tiled_params; make_params leaves it 0).
    f32 blend_pad = 0.f;
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

/// Unnormalised direction of the ray through the centre of pixel (x, y) (row 0 = top).
FUSE_HOST_DEVICE inline math::Vec3 pixel_direction(const Params& p, f32 x, f32 y) {
    const RayMarchParams& s = p.scene;
    const f32 ndcX = (2.f * (x + 0.5f) / static_cast<f32>(s.width) - 1.f);
    const f32 ndcY = (1.f - 2.f * (y + 0.5f) / static_cast<f32>(s.height));
    return p.forward + p.right * (ndcX * p.tan_half * p.aspect) + p.up * (ndcY * p.tan_half);
}

/// Traces pixel (x, y) through `objects` and writes its depth + world normal.
template <typename Objects>
FUSE_HOST_DEVICE inline void shade_pixel(const Params& p, Objects objects, u32 x, u32 y) {
    const RayMarchParams& s = p.scene;
    const math::Vec3 direction = pixel_direction(p, static_cast<f32>(x), static_cast<f32>(y)).normalized();
    const f32 t = hit_distance_in(s, objects, s.cam_pos, direction);
    const u32 pixel = y * s.width + x;
    if (s.depth_surface != nullptr) {
        static_cast<f32*>(s.depth_surface)[pixel] = t;
    }
    if (s.output_surface != nullptr) {
        static_cast<math::Vec4*>(s.output_surface)[pixel] =
            t >= 0.f ? math::Vec4{scene_normal_in(s, objects, s.cam_pos + direction * t), 1.f} : math::Vec4{};
    }
}

/// The kernel body: one pixel ray through the pixel centre (row 0 = top) -> depth + world normal.
struct Kernel {
    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const Params& p) const {
        shade_pixel(p, AllObjects{p.scene.object_count}, idx.global.x, idx.global.y);
    }
};

// ---- tiled march: per-tile object culling --------------------------------------------------------------
//
// "sdf_ray_march_tiled": one workgroup per 16x8 pixel tile. The workgroup first culls the scene against
// the tile's view pyramid (the four planes through the camera and the tile's corner pixel rays, plus
// max_dist) into an ordered list in workgroup scratch, then every pixel marches only that list:
//   phase 0  thread 0 builds the tile's view pyramid into scratch;
//   phase 1  each thread tests a contiguous chunk of objects and counts the ones that may matter;
//   phase 2  thread 0 turns the 128 counts into offsets (exclusive scan, thread order);
//   phase 3  each thread writes its chunk's survivors at its offset -> ascending object order;
//   phase 4  march + normal over the list (or over the whole scene if the list overflowed).
// An object is kept when its bounding sphere, grown by Params::blend_pad and a 1e-4 relative float
// margin, reaches the pyramid. A culled object cannot be hit by any ray of the tile, and its distance
// only ever shortened the sphere-tracing steps, so the tile shows the same surfaces; hit distances
// differ from the full-scene march only by where the trace stops inside min_dist (and a grazing ray may
// converge where the full march ran out of steps). CpuReference / CpuParallel / CUDA run this body.

inline constexpr const char* kTiledName = "sdf_ray_march_tiled";

// nvcc: keep the culling loops rolled. Unrolled, they interleave several objects' loads and plane tests
// and the tiled kernel needs ~180 registers instead of fitting the march's budget.
// The tile pyramid is built by a real call on the device: inlined, nvcc hoists that pure, loop-invariant
// math out of the workgroup's phase loop and keeps it live (in registers or spills) through the march.
#if defined(__CUDA_ARCH__)
#define FUSE_RAY_MARCH_ROLLED_LOOP _Pragma("unroll 1")
#define FUSE_RAY_MARCH_NOINLINE __noinline__
#else
#define FUSE_RAY_MARCH_ROLLED_LOOP
#define FUSE_RAY_MARCH_NOINLINE
#endif
inline constexpr u32 kTileThreads = kWorkgroup.x * kWorkgroup.y;
/// Objects a tile list holds; a tile whose pyramid meets more marches the whole scene instead.
inline constexpr u32 kTileListCapacity = 1024u;

// The tiled-march helpers use value min / max: std::min / std::max / std::clamp return references, and
// nvcc then keeps the bounds in local memory (as it did for clamped_rounding).
FUSE_HOST_DEVICE inline f32 positive_part(f32 v) {
    return v > 0.f ? v : 0.f;
}

FUSE_HOST_DEVICE inline u32 min_u32(u32 a, u32 b) {
    return a < b ? a : b;
}

/// Radius of a sphere around obj.position containing the primitive (+inf: never culled).
FUSE_HOST_DEVICE inline f32 bounding_radius(const SdfObject& obj) {
    switch (static_cast<SdfPrimitiveType>(obj.type)) {
    case SdfPrimitiveType::Sphere:
        return positive_part(obj.params.x);
    case SdfPrimitiveType::Box:
        return math::Vec3{std::abs(obj.params.x), std::abs(obj.params.y), std::abs(obj.params.z)}.length();
    case SdfPrimitiveType::Capsule:
        return positive_part(obj.params.y) + positive_part(obj.params.x);
    case SdfPrimitiveType::Torus:
        return std::abs(obj.params.x) + positive_part(obj.params.y);
    default:
        return 3.40282347e+38f;
    }
}

/// The view pyramid of one tile: inward unit normals of the planes through the camera and the tile's
/// corner pixel-centre rays (every pixel ray of the tile is a positive combination of the corner rays).
struct TileFrustum {
    math::Vec3 normal[4];
};

/// Tile pyramid inputs: the camera basis and the tile's pixel-centre bounds (a small by-value argument:
/// a device-side call cannot take the kernel's Params by reference without copying them to the stack).
struct TileRect {
    math::Vec3 forward{};
    math::Vec3 right{};
    math::Vec3 up{};
    f32 a0 = 0.f; ///< right-axis slope of the tile's first / last pixel-centre column
    f32 a1 = 0.f;
    f32 b0 = 0.f; ///< up-axis slope of the tile's bottom / top pixel-centre row
    f32 b1 = 0.f;
};

FUSE_HOST_DEVICE inline TileRect tile_rect(const Params& p, const kernel::Dim3& group) {
    const RayMarchParams& s = p.scene;
    const u32 x0 = group.x * kWorkgroup.x;
    const u32 y0 = group.y * kWorkgroup.y;
    const f32 x1 = static_cast<f32>(min_u32(x0 + kWorkgroup.x, s.width) - 1u);
    const f32 y1 = static_cast<f32>(min_u32(y0 + kWorkgroup.y, s.height) - 1u);
    const f32 w = static_cast<f32>(s.width);
    const f32 h = static_cast<f32>(s.height);
    TileRect r{};
    r.forward = p.forward;
    r.right = p.right;
    r.up = p.up;
    r.a0 = (2.f * (static_cast<f32>(x0) + 0.5f) / w - 1.f) * p.tan_half * p.aspect;
    r.a1 = (2.f * (x1 + 0.5f) / w - 1.f) * p.tan_half * p.aspect;
    r.b0 = (1.f - 2.f * (y1 + 0.5f) / h) * p.tan_half;
    r.b1 = (1.f - 2.f * (static_cast<f32>(y0) + 0.5f) / h) * p.tan_half;
    return r;
}

/// Pixel rays are D(a, b) = forward + right a + up b (then normalised) with a, b affine in the pixel
/// centre; over the tile a in [a0, a1], b in [b0, b1]. The plane through the camera and the edge a = a0
/// contains D(a0, b0) and D(a0, b1), so its normal is cross(forward + right a0, up); likewise for a1 and,
/// with right, for b0 / b1. Each is oriented towards the tile's centre ray.
FUSE_RAY_MARCH_NOINLINE FUSE_HOST_DEVICE inline TileFrustum tile_frustum(TileRect r) {
    const math::Vec3 centre = r.forward + r.right * (0.5f * (r.a0 + r.a1)) + r.up * (0.5f * (r.b0 + r.b1));
    TileFrustum f{};
    f.normal[0] = math::cross(r.forward + r.right * r.a0, r.up);
    f.normal[1] = math::cross(r.forward + r.right * r.a1, r.up);
    f.normal[2] = math::cross(r.forward + r.up * r.b0, r.right);
    f.normal[3] = math::cross(r.forward + r.up * r.b1, r.right);
    for (int i = 0; i < 4; ++i) {
        const f32 length = f.normal[i].length();
        const f32 sign = f.normal[i].dot(centre) < 0.f ? -1.f : 1.f;
        f.normal[i] = length > 0.f ? f.normal[i] * (sign / length) : math::Vec3{};
    }
    return f;
}

/// True when object `i` may affect the tile's pixels (see the tiled-march comment).
FUSE_HOST_DEVICE inline bool tile_keeps(const Params& p, const TileFrustum& f, u32 i) {
    const SdfObject& obj = p.scene.objects[i];
    const f32 bound = bounding_radius(obj);
    if (!(bound < 1e30f)) {
        return true;
    }
    const math::Vec3 c = obj.position - p.scene.cam_pos;
    const f32 distance = c.length();
    const f32 r = bound + p.blend_pad + 1e-4f * (distance + bound) + 1e-5f;
    if (distance - r > p.scene.max_dist) {
        return false;
    }
    for (int k = 0; k < 4; ++k) {
        if (f.normal[k].dot(c) < -r) {
            return false;
        }
    }
    return true;
}

FUSE_HOST_DEVICE inline u32 float_bits(f32 v) {
#if defined(__CUDA_ARCH__)
    return static_cast<u32>(__float_as_uint(v));
#else
    return std::bit_cast<u32>(v);
#endif
}

FUSE_HOST_DEVICE inline f32 bits_float(u32 v) {
#if defined(__CUDA_ARCH__)
    return __uint_as_float(v);
#else
    return std::bit_cast<f32>(v);
#endif
}

struct TiledKernel {
    using Scratch = u32;
    /// Scratch layout (u32 words): per-thread counts -> offsets, the list length, the tile pyramid's four
    /// unit normals (float bits), then the list.
    static constexpr u32 kCounts = 0u;
    static constexpr u32 kKept = kTileThreads;
    static constexpr u32 kPlanes = kKept + 1u;
    static constexpr u32 kList = kPlanes + 12u;
    static constexpr u32 kScratchCount = kList + kTileListCapacity;
    static constexpr u32 kPhases = 5u;

    /// The pyramid is built once per tile (phase 0) and read back from scratch: recomputed in the
    /// culling phases, nvcc hoists it out of the phase loop and keeps it live through the march.
    FUSE_HOST_DEVICE static TileFrustum load_frustum(const u32* scratch) {
        TileFrustum f{};
        for (int k = 0; k < 4; ++k) {
            f.normal[k] = math::Vec3{bits_float(scratch[kPlanes + 3u * k]), bits_float(scratch[kPlanes + 3u * k + 1u]),
                                     bits_float(scratch[kPlanes + 3u * k + 2u])};
        }
        return f;
    }

    FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const kernel::WorkgroupContext<u32>& wg,
                                     const Params& p) const {
        u32* scratch = wg.scratch;
        const u32 local = idx.local_linear;
        const u32 n = p.scene.object_count;
        const u32 chunk = kernel::div_up(n, kTileThreads);
        const u32 begin = min_u32(local * chunk, n);
        const u32 end = min_u32(begin + chunk, n);
        switch (wg.phase) {
        case 0:
            // The tile's view pyramid, once.
            if (local == 0u) {
                const TileFrustum f = tile_frustum(tile_rect(p, idx.group));
                for (int k = 0; k < 4; ++k) {
                    scratch[kPlanes + 3u * k] = float_bits(f.normal[k].x);
                    scratch[kPlanes + 3u * k + 1u] = float_bits(f.normal[k].y);
                    scratch[kPlanes + 3u * k + 2u] = float_bits(f.normal[k].z);
                }
            }
            break;
        case 1: {
            // Count: this thread's contiguous chunk of objects.
            const TileFrustum f = load_frustum(scratch);
            u32 kept = 0u;
            FUSE_RAY_MARCH_ROLLED_LOOP
            for (u32 i = begin; i < end; ++i) {
                kept += tile_keeps(p, f, i) ? 1u : 0u;
            }
            scratch[kCounts + local] = kept;
            break;
        }
        case 2:
            // Scan: serial and in thread order, identical on every backend.
            if (local == 0u) {
                u32 running = 0u;
                FUSE_RAY_MARCH_ROLLED_LOOP
                for (u32 t = 0; t < kTileThreads; ++t) {
                    const u32 kept = scratch[kCounts + t];
                    scratch[kCounts + t] = running;
                    running += kept;
                }
                scratch[kKept] = running;
            }
            break;
        case 3:
            // Write: survivors at the thread's offset -> the list is in ascending object order.
            if (scratch[kKept] <= kTileListCapacity) {
                const TileFrustum f = load_frustum(scratch);
                u32 slot = kList + scratch[kCounts + local];
                FUSE_RAY_MARCH_ROLLED_LOOP
                for (u32 i = begin; i < end; ++i) {
                    if (tile_keeps(p, f, i)) {
                        scratch[slot++] = i;
                    }
                }
            }
            break;
        default:
            if (idx.active) {
                const u32 kept = scratch[kKept];
                const ObjectList objects =
                    kept <= kTileListCapacity ? ObjectList{scratch + kList, kept} : ObjectList{nullptr, n};
                shade_pixel(p, objects, idx.global.x, idx.global.y);
            }
            break;
        }
    }
};

/// make_params plus the tiled march's blend_pad, read from the HOST copy of the objects (`scene.objects`
/// may already point at device memory).
inline Params make_tiled_params(const RayMarchParams& scene, const SdfObject* hostObjects) {
    Params p = make_params(scene);
    for (u32 i = 0; i < scene.object_count; ++i) {
        p.blend_pad = std::max(p.blend_pad, 2.f * hostObjects[i].alpha);
    }
    return p;
}

inline kernel::KernelLaunch make_tiled_launch(const RayMarchParams& scene) {
    return kernel::KernelLaunch{kTiledName, kernel::extent2(scene.width, scene.height), kWorkgroup};
}

} // namespace fuse::compute::ray_march_kernel

#undef FUSE_RAY_MARCH_ROLLED_LOOP
#undef FUSE_RAY_MARCH_NOINLINE
