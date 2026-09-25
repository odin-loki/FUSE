// CUDA backend of SSAO: the __global__ trampolines from cuda_launch.cuh run the same FUSE_HOST_DEVICE bodies
// the CPU backends run — ssfx::hbao_kernel ("screen_space_ao") then the cross-bilateral blur
// ("screen_space_ao_blur", fuse/compute/screen_space_kernels.hpp). This TU only stages the surfaces in device
// memory (the ray_march.cu pattern).

#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::compute {

bool launchSsaoCuda(const SSAOParams& params, void* stream) {
    namespace ssk = screen_space_kernels;
    namespace hbao = ssfx::hbao_kernel;
    ssfx::SsfxGBufferView hostView{};
    if (!validate_ssao_params(params) || params.ao_out_surface == nullptr ||
        !ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface,
                        hostView)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(params.width) * params.height;

    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<f32> raw;
    kernel::cuda::DeviceBuffer<f32> out;
    bool ok = depth.allocate(pixels) && depth.upload(hostView.depth, pixels, cudaStream) && out.allocate(pixels);
    if (ok && hostView.normals != nullptr) {
        ok = normals.allocate(pixels) && normals.upload(hostView.normals, pixels, cudaStream);
    }
    if (ok && params.enable_blur) {
        ok = raw.allocate(pixels);
    }
    if (!ok) {
        return false;
    }

    ssfx::SsfxGBufferView deviceView = hostView;
    deviceView.depth = depth.data();
    deviceView.normals = hostView.normals != nullptr ? normals.data() : nullptr;

    kernel::LaunchOptions options{};
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = false; // Same stream: the blur and the download are ordered after the AO launch.

    options.cuda = &kernel::cuda::entry<hbao::Kernel, hbao::Params>;
    f32* const aoTarget = params.enable_blur ? raw.data() : out.data();
    ok = kernel::launch(kernel::Backend::Cuda, hbao::make_launch(deviceView), hbao::Kernel{},
                        hbao::make_params(deviceView, ssk::to_hbao(params), aoTarget), options)
             .ok;
    if (ok && params.enable_blur) {
        options.cuda = &kernel::cuda::entry<ssk::ao_blur::Kernel, ssk::ao_blur::Params>;
        const ssk::ao_blur::Params blur{deviceView, ssk::ao_blur::thresholds(params), raw.data(), out.data()};
        ok = kernel::launch(kernel::Backend::Cuda, ssk::ao_blur::make_launch(deviceView), ssk::ao_blur::Kernel{},
                            blur, options)
                 .ok;
    }
    if (ok) {
        ok = out.download(static_cast<f32*>(params.ao_out_surface), pixels, cudaStream);
    }
    return cudaStreamSynchronize(cudaStream) == cudaSuccess && ok;
}

} // namespace fuse::compute
