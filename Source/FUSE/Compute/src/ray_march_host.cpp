#include <fuse/compute/ray_march.hpp>
#include <fuse/compute_kernel/stats.hpp>

namespace fuse::compute {

#if defined(FUSE_HAS_CUDA)
/// kernels/ray_march.cu: stages the scene + surfaces on the device and runs the same kernel body.
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
    info.device_available = kernel::backend_available(kernel::Backend::Cuda);
    return info;
}

bool launch_ray_march_on(kernel::Backend backend, const RayMarchParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda)) {
        return launchRayMarchCuda(params, stream);
    }
#else
    (void)stream;
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend.
    return launch_ray_march_cpu_backend(backend, params);
}

bool launch_ray_march(const RayMarchParams& params, void* stream) {
    return launch_ray_march_on(kernel::Backend::Auto, params, stream);
}

} // namespace fuse::compute
