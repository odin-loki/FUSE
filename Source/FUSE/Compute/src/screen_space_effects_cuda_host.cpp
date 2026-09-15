#include <fuse/compute/screen_space_effects.hpp>

#if defined(FUSE_HAS_CUDA)

namespace fuse::compute {

namespace {

extern "C" void launch_ssao_kernel_stub(void* stream);
extern "C" void launch_ssr_kernel_stub(void* stream);
extern "C" void launch_ssgi_kernel_stub(void* stream);

} // namespace

bool launchSsaoCuda(const SSAOParams& params, void* stream) {
    (void)params;
    launch_ssao_kernel_stub(stream);
    return true;
}

bool launchSsrCuda(const SSRParams& params, void* stream) {
    (void)params;
    launch_ssr_kernel_stub(stream);
    return true;
}

bool launchSsgiCuda(const SSGIParams& params, void* stream) {
    (void)params;
    launch_ssgi_kernel_stub(stream);
    return true;
}

} // namespace fuse::compute

#endif
