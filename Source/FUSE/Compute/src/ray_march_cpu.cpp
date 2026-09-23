// CPU entry points of the SDF ray march. All SDF / trace / normal math lives once in
// fuse/compute/ray_march_kernel.hpp (FUSE_HOST_DEVICE, shared with kernels/ray_march.cu); this TU only
// adapts the public API onto it and launches the kernel body on the CPU backends.

#include <fuse/compute/ray_march.hpp>
#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute_kernel/launch.hpp>

namespace fuse::compute {

f32 ray_march_scene_distance(const RayMarchParams& params, const math::Vec3& position) {
    return ray_march_kernel::scene_eval(params, position, nullptr);
}

math::Vec3 ray_march_scene_normal(const RayMarchParams& params, const math::Vec3& position) {
    return ray_march_kernel::scene_normal(params, position);
}

f32 ray_march_hit_distance(const RayMarchParams& params, const math::Vec3& origin, const math::Vec3& direction) {
    return ray_march_kernel::hit_distance(params, origin, direction);
}

f32 ray_march_center_hit_distance(const RayMarchParams& params) {
    return ray_march_hit_distance(params, params.cam_pos, params.cam_forward);
}

bool launch_ray_march_cpu_backend(kernel::Backend backend, const RayMarchParams& params) {
    if (!ray_march_kernel::params_valid(params)) {
        return false;
    }
    return kernel::launch(backend, ray_march_kernel::make_launch(params), ray_march_kernel::Kernel{},
                          ray_march_kernel::make_params(params))
        .ok;
}

bool launch_ray_march_cpu(const RayMarchParams& params) {
    return launch_ray_march_cpu_backend(kernel::Backend::CpuReference, params);
}

} // namespace fuse::compute
