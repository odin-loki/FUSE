// Host entry of the `sdf_shadows` pass. The march and the scene SDF live in FUSE_HOST_DEVICE headers
// (shadow/sdf_shadow_kernel.hpp -> sdf_soft_shadow.hpp + compute/ray_march_kernel.hpp); this TU validates,
// routes to CUDA when a device exists, and otherwise launches the body on the CPU backends.

#include <fuse/renderer/shadow/sdf_shadows.hpp>

#include <fuse/compute_kernel/launch.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/renderer/shadow/sdf_shadow_kernel.hpp>

namespace fuse::renderer {

#if defined(FUSE_HAS_CUDA)
/// kernels/sdf_shadows.cu: stages the frame on the device and runs the same kernel body.
bool launchSdfShadowsCuda(const SdfShadowFrame& frame, void* stream);
#endif

bool sdfShadowFrameValid(const SdfShadowFrame& frame) {
    return sdf_shadow_kernel::params_valid(frame);
}

bool renderSdfShadows(const SdfShadowFrame& frame, kernel::Backend backend, void* stream) {
    if (!sdf_shadow_kernel::params_valid(frame)) {
        return false;
    }
#if defined(FUSE_HAS_CUDA)
    if ((backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
        kernel::backend_available(kernel::Backend::Cuda)) {
        return launchSdfShadowsCuda(frame, stream);
    }
#else
    (void)stream;
#endif
    // CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback
    // (CpuParallel) and records the requested vs executed backend.
    return kernel::launch(backend, sdf_shadow_kernel::make_launch(frame), sdf_shadow_kernel::Kernel{},
                          sdf_shadow_kernel::make_params(frame))
        .ok;
}

} // namespace fuse::renderer
