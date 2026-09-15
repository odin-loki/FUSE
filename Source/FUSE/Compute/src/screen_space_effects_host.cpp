#include <fuse/compute/screen_space_effects.hpp>

namespace fuse::compute {

bool launch_ssao_cpu(const SSAOParams& params);
bool launch_ssr_cpu(const SSRParams& params);
bool launch_ssgi_cpu(const SSGIParams& params);

#if defined(FUSE_HAS_CUDA)
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
    return info;
}

bool launch_ssao(const SSAOParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    return launchSsaoCuda(params, stream);
#else
    (void)stream;
    return launch_ssao_cpu(params);
#endif
}

bool launch_ssr(const SSRParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    return launchSsrCuda(params, stream);
#else
    (void)stream;
    return launch_ssr_cpu(params);
#endif
}

bool launch_ssgi(const SSGIParams& params, void* stream) {
#if defined(FUSE_HAS_CUDA)
    return launchSsgiCuda(params, stream);
#else
    (void)stream;
    return launch_ssgi_cpu(params);
#endif
}

} // namespace fuse::compute
