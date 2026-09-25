#pragma once

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::compute {

enum class SdfPrimitiveType : u8 { Sphere = 0, Box = 1, Capsule = 2, Torus = 3 };

/// Analytic SDF primitive (local frame axis-aligned, centred on `position`). `params` by type:
/// Sphere  — x = radius;
/// Box     — xyz = half extents (outer extents; edges/corners rounded by `rounding`);
/// Capsule — x = radius, y = half length of the core segment along local Y (0 = sphere);
/// Torus   — x = major radius (ring in the local XZ plane), y = minor (tube) radius.
struct SdfObject {
    math::Vec3 position{};
    math::Vec3 params{};
    u32 type = 0;
    /// Smooth-union blend width with the objects before it (<= 0 = hard union).
    f32 alpha = 0.1f;
    u32 material_id = 0;
    /// Box only: edge/corner rounding radius (0 = sharp). The rounded box keeps `params` as its outer half
    /// extents (`box(p, params - rounding) - rounding`); clamped to the smallest half extent.
    f32 rounding = 0.f;
};

/// CPU reference surfaces (host arrays of `width * height`, row 0 = top): `depth_surface` is `f32*` hit distance
/// along the pixel ray (-1 on a miss); `output_surface` is `math::Vec4*` world-space normal xyz with w = 1 on a
/// hit (all zero on a miss). Either may be null. Pixel rays go through pixel centres with vertical FOV `fov_rad`.
struct RayMarchParams {
    void* output_surface = nullptr;
    void* depth_surface = nullptr;
    u32 width = 0;
    u32 height = 0;

    math::Vec3 cam_pos{};
    math::Vec3 cam_forward{0.f, 0.f, 1.f};
    math::Vec3 cam_right{1.f, 0.f, 0.f};
    math::Vec3 cam_up{0.f, 1.f, 0.f};
    f32 fov_rad = 1.0f;

    u32 max_steps = 128;
    f32 min_dist = 0.0001f;
    f32 max_dist = 1000.f;
    f32 alpha = 0.f;

    const SdfObject* objects = nullptr;
    u32 object_count = 0;
};

enum class RayMarcherMode : u8 { Stub, CpuReference, Cuda };

struct RayMarcherInfo {
    bool valid = false;
    /// Backend compiled in (Cuda when built with FUSE_HAS_CUDA).
    RayMarcherMode mode = RayMarcherMode::Stub;
    /// A CUDA device is usable right now; without one every launch falls back to the CPU backends.
    bool device_available = false;
};

RayMarcherInfo ray_marcher_info();

/// Host launcher: `launch_ray_march_on(Backend::Auto, ...)` — CUDA when built with `FUSE_HAS_CUDA` and a
/// device is present, CpuParallel otherwise. The kernel body is fuse/compute/ray_march_kernel.hpp.
bool launch_ray_march(const RayMarchParams& params, void* stream = nullptr);

/// Full-frame launch on an explicit backend (kernel / profiler / stats name "sdf_ray_march"). GPU
/// backends that cannot run here fall back to CpuParallel. False on invalid parameters.
bool launch_ray_march_on(kernel::Backend backend, const RayMarchParams& params, void* stream = nullptr);

/// CPU-only form of launch_ray_march_on (compiled without any CUDA dependency).
bool launch_ray_march_cpu_backend(kernel::Backend backend, const RayMarchParams& params);

/// CPU sphere-tracing reference for unit tests (returns hit distance, or -1 on miss).
f32 ray_march_center_hit_distance(const RayMarchParams& params);

/// CPU sphere trace of one ray against the scene (`direction` need not be unit); hit distance or -1.
f32 ray_march_hit_distance(const RayMarchParams& params, const math::Vec3& origin, const math::Vec3& direction);

/// Signed distance of the scene at `position` (hard or smooth union of all objects).
f32 ray_march_scene_distance(const RayMarchParams& params, const math::Vec3& position);

/// Unit surface normal of the scene at `position`: the analytic gradient of `ray_march_scene_distance`
/// (per-primitive closed-form gradients chained through the smooth union's derivative). Unlike a
/// finite-difference normal it has no step size, so it cannot facet or quantise into plateaus at any
/// zoom level: its error is ~1 f32 ulp of `position` relative to the local feature radius.
/// Returns +Y where the gradient vanishes (empty scene / medial singularity).
math::Vec3 ray_march_scene_normal(const RayMarchParams& params, const math::Vec3& position);

/// CPU reference (serial, deterministic) full-frame trace into the host surfaces; false on invalid parameters.
bool launch_ray_march_cpu(const RayMarchParams& params);

} // namespace fuse::compute
