#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute_kernel/stats.hpp>

namespace fuse::compute {

#if defined(FUSE_HAS_CUDA)
/// kernels/{ssao,ssr,ssgi}.cu: stage the host surfaces on the device and run the same kernel bodies.
bool launchSsaoCuda(const SSAOParams& params, void* stream);
bool launchSsrCuda(const SSRParams& params, void* stream);
bool launchSsgiCuda(const SSGIParams& params, void* stream);
#endif

ScreenSpaceEffectsInfo screen_space_effects_info() {
    ScreenSpaceEffectsInfo info{};
    info.valid = true;
#if defined(FUSE_HAS_CUDA)
    info.mode = ScreenSpaceEffectsMode::Cuda;
#else
    info.mode = ScreenSpaceEffectsMode::CpuReference;
#endif
    info.device_available = kernel::backend_available(kernel::Backend::Cuda);
    return info;
}

namespace {

[[maybe_unused]] bool wantsDevice(kernel::Backend backend) {
    return (backend == kernel::Backend::Cuda || backend == kernel::Backend::Auto) &&
           kernel::backend_available(kernel::Backend::Cuda);
}

} // namespace

// CPU backends, or a GPU backend that cannot run here: kernel::launch resolves the fallback (CpuParallel) and
// records the requested vs executed backend.

bool launch_ssao_on(kernel::Backend backend, const SSAOParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    if (wantsDevice(backend)) {
        return launchSsaoCuda(params, stream);
    }
#endif
    (void)stream;
    return launch_ssao_cpu_backend(backend, params);
}

bool launch_ssr_on(kernel::Backend backend, const SSRParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    if (wantsDevice(backend)) {
        return launchSsrCuda(params, stream);
    }
#endif
    (void)stream;
    return launch_ssr_cpu_backend(backend, params);
}

bool launch_ssgi_on(kernel::Backend backend, const SSGIParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    if (wantsDevice(backend)) {
        return launchSsgiCuda(params, stream);
    }
#endif
    (void)stream;
    return launch_ssgi_cpu_backend(backend, params);
}

bool launch_ssao(const SSAOParams& params, void* stream) {
    return launch_ssao_on(kernel::Backend::Auto, params, stream);
}

bool launch_ssr(const SSRParams& params, void* stream) {
    return launch_ssr_on(kernel::Backend::Auto, params, stream);
}

bool launch_ssgi(const SSGIParams& params, void* stream) {
    return launch_ssgi_on(kernel::Backend::Auto, params, stream);
}

} // namespace fuse::compute
