#include <fuse/compute/ray_march.hpp>

#if defined(FUSE_HAS_CUDA)

namespace fuse::compute {

namespace {

extern "C" void launch_ray_march_kernel_stub(void* stream);

} // namespace

bool launchRayMarchCuda(const RayMarchParams& params, void* stream) {
    (void)params;
    launch_ray_march_kernel_stub(stream);
    return true;
}

} // namespace fuse::compute

#endif
