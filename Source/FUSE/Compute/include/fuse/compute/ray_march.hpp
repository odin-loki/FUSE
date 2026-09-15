#pragma once

#include <fuse/math/vec.hpp>
#include <fuse/types.hpp>

namespace fuse::compute {

enum class SdfPrimitiveType : u8 { Sphere = 0, Box = 1, Capsule = 2, Torus = 3 };

struct SdfObject {
    math::Vec3 position{};
    math::Vec3 params{};
    u32 type = 0;
    f32 alpha = 0.1f;
    u32 material_id = 0;
};

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
    RayMarcherMode mode = RayMarcherMode::Stub;
};

RayMarcherInfo ray_marcher_info();

/// Host launcher — CUDA path when `FUSE_HAS_CUDA=1`, CPU reference otherwise.
bool launch_ray_march(const RayMarchParams& params, void* stream = nullptr);

/// CPU sphere-tracing reference for unit tests (returns hit distance, or -1 on miss).
f32 ray_march_center_hit_distance(const RayMarchParams& params);

} // namespace fuse::compute
