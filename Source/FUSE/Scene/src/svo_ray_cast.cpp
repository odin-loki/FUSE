// Batched SVO ray cast: CPU launches of the single-source "svo_ray_cast" kernel
// (fuse/scene/svo_ray_kernel.hpp) plus the Auto / Cuda routing. The per-ray walk itself lives only in
// the kernel header; SVO::rayCast calls the same function for one ray.

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/scene/svo.hpp>
#include <fuse/scene/svo_ray_kernel.hpp>

namespace fuse::scene {

#if defined(FUSE_HAS_CUDA)
/// kernels/svo_ray_cast.cu: stages the view + rays on the device and runs the same kernel body.
bool launchSvoRayCastCuda(const svo_kernel::Params& params, void* stream);
#endif

bool SVO::rayCastBatch(kernel::Backend backend, const SvoRay* rays, SvoRayHit* hits, u32 count, void* stream) const {
    svo_kernel::Params params{};
    params.svo = view();
    params.rays = kernel::make_span(rays, count);
    params.hits = kernel::make_span(hits, count);
    if (!svo_kernel::params_valid(params)) {
        return false;
    }
#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda)) {
        return launchSvoRayCastCuda(params, stream);
    }
#else
    (void)stream;
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend.
    return kernel::launch(backend, svo_kernel::make_launch(count), svo_kernel::Kernel{}, params).ok;
}

} // namespace fuse::scene
