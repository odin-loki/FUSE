// CUDA backend of SSGI: the __global__ trampoline from cuda_launch.cuh runs the same FUSE_HOST_DEVICE body the
// CPU backends run (ssfx::ssgi_kernel, "screen_space_gi"), driven bounce by bounce by the shared
// ssgi_kernel::run_bounces. This TU only stages the surfaces and the radiance ping-pong in device memory (the
// ray_march.cu pattern).

#include <fuse/compute/screen_space_contact.hpp>
#include <fuse/compute/screen_space_effects.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

namespace fuse::compute {

bool launchSsgiCuda(const SSGIParams& params, void* stream) {
    namespace ssk = screen_space_kernels;
    namespace ssgi = ssfx::ssgi_kernel;
    ssfx::SsfxGBufferView hostView{};
    if (!validate_ssgi_params(params) || params.scene_color_surface == nullptr ||
        params.ssgi_out_surface == nullptr ||
        !ssk::make_view(params.proj, params.width, params.height, params.depth_surface, params.normal_surface,
                        hostView)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(params.width) * params.height;
    const ssfx::SsgiParams clamped = ssgi::clamp_params(ssk::to_ssgi(params));
    const auto* hostAlbedo = static_cast<const math::Vec3*>(params.albedo_surface);

    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<math::Vec3> direct;
    kernel::cuda::DeviceBuffer<math::Vec3> albedo;
    kernel::cuda::DeviceBuffer<math::Vec3> out;
    kernel::cuda::DeviceBuffer<math::Vec3> scratch0;
    kernel::cuda::DeviceBuffer<math::Vec3> scratch1;
    const u32 scratchCount = ssgi::scratch_buffers(clamped.bounces);
    bool ok = depth.allocate(pixels) && depth.upload(hostView.depth, pixels, cudaStream) &&
              direct.allocate(pixels) &&
              direct.upload(static_cast<const math::Vec3*>(params.scene_color_surface), pixels, cudaStream) &&
              out.allocate(pixels) && (scratchCount < 1u || scratch0.allocate(pixels)) &&
              (scratchCount < 2u || scratch1.allocate(pixels));
    if (ok && hostView.normals != nullptr) {
        ok = normals.allocate(pixels) && normals.upload(hostView.normals, pixels, cudaStream);
    }
    if (ok && hostAlbedo != nullptr) {
        ok = albedo.allocate(pixels) && albedo.upload(hostAlbedo, pixels, cudaStream);
    }
    if (!ok) {
        return false;
    }

    ssfx::SsfxGBufferView deviceView = hostView;
    deviceView.depth = depth.data();
    deviceView.normals = hostView.normals != nullptr ? normals.data() : nullptr;

    if (clamped.bounces == 0u) {
        ok = cudaMemsetAsync(out.data(), 0, pixels * sizeof(math::Vec3), cudaStream) == cudaSuccess;
    } else {
        ssgi::Params base{};
        base.view = deviceView;
        base.ssgi = clamped;
        base.direct = direct.data();
        base.albedo = hostAlbedo != nullptr ? albedo.data() : nullptr;
        base.indirect_out = out.data();

        kernel::LaunchOptions options{};
        options.cuda = &kernel::cuda::entry<ssgi::Kernel, ssgi::Params>;
        options.stream = stream;
        options.allow_fallback = false;
        options.synchronize = false; // Bounces and the download are ordered on the same stream.
        ok = ssgi::run_bounces(base, scratch0.data(), scratch1.data(), [&](const ssgi::Params& bounce) {
            return kernel::launch(kernel::Backend::Cuda, ssgi::make_launch(deviceView), ssgi::Kernel{}, bounce,
                                  options)
                .ok;
        });
    }
    if (ok) {
        ok = out.download(static_cast<math::Vec3*>(params.ssgi_out_surface), pixels, cudaStream);
    }
    return cudaStreamSynchronize(cudaStream) == cudaSuccess && ok;
}

} // namespace fuse::compute
