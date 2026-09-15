#include <fuse/compute/ray_march.hpp>

namespace fuse::compute {

bool launch_ray_march_cpu(const RayMarchParams& params);

#if defined(FUSE_HAS_CUDA)
bool launchRayMarchCuda(const RayMarchParams& params, void* stream);
#endif

RayMarcherInfo ray_marcher_info() {
    RayMarcherInfo info{};
    info.valid = true;
#if defined(FUSE_HAS_CUDA)
    info.mode = RayMarcherMode::Cuda;
#else
    info.mode = RayMarcherMode::CpuReference;
#endif
    return info;
}

bool launch_ray_march(const RayMarchParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    return launchRayMarchCuda(params, stream);
#else
    (void)stream;
    return launch_ray_march_cpu(params);
#endif
}

} // namespace fuse::compute
