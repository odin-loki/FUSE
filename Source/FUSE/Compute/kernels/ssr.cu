// CUDA backend of SSR: the __global__ trampoline from cuda_launch.cuh runs the same FUSE_HOST_DEVICE body the
// CPU backends run (ssfx::ssr_kernel, "screen_space_reflections"). This TU only stages the surfaces in device
// memory (the ray_march.cu pattern).

#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::compute {

bool launchSsrCuda(const SSRParams& params, void* stream) {
    namespace ssk = screen_space_kernels;
    namespace ssr = ssfx::ssr_kernel;
    ssfx::SsfxGBufferView hostView{};
    if (!validate_ssr_params(params) || params.scene_color_surface == nullptr || params.ssr_out_surface == nullptr ||
        !ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface,
                        hostView)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(params.width) * params.height;
    const auto* hostRoughness = static_cast<const f32*>(params.roughness_surface);

    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<math::Vec3> color;
    kernel::cuda::DeviceBuffer<f32> roughness;
    kernel::cuda::DeviceBuffer<math::Vec4> out;
    bool ok = depth.allocate(pixels) && depth.upload(hostView.depth, pixels, cudaStream) && color.allocate(pixels) &&
              color.upload(static_cast<const math::Vec3*>(params.scene_color_surface), pixels, cudaStream) &&
              out.allocate(pixels);
    if (ok && hostView.normals != nullptr) {
        ok = normals.allocate(pixels) && normals.upload(hostView.normals, pixels, cudaStream);
    }
    if (ok && hostRoughness != nullptr) {
        ok = roughness.allocate(pixels) && roughness.upload(hostRoughness, pixels, cudaStream);
    }
    if (!ok) {
        return false;
    }

    ssfx::SsfxGBufferView deviceView = hostView;
    deviceView.depth = depth.data();
    deviceView.normals = hostView.normals != nullptr ? normals.data() : nullptr;

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<ssr::Kernel, ssr::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = false; // The download below is ordered on the same stream.
    ok = kernel::launch(kernel::Backend::Cuda, ssr::make_launch(deviceView), ssr::Kernel{},
                        ssk::make_ssr_params(deviceView, params, color.data(),
                                             hostRoughness != nullptr ? roughness.data() : nullptr, out.data()),
                        options)
             .ok;
    if (ok) {
        ok = out.download(static_cast<math::Vec4*>(params.ssr_out_surface), pixels, cudaStream);
    }
    return cudaStreamSynchronize(cudaStream) == cudaSuccess && ok;
}

} // namespace fuse::compute
