// CUDA backend of the `sdf_shadows` pass: the item trampoline runs the same FUSE_HOST_DEVICE body
// (fuse/renderer/shadow/sdf_shadow_kernel.hpp -> compute::ray_march_kernel::scene_eval + sdfSoftShadow).
// This TU only stages the frame in device memory and reads the shadow factors back.

#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/shadow/sdf_shadow_kernel.hpp>

#include <cuda_runtime.h>

namespace fuse::renderer {

bool launchSdfShadowsCuda(const SdfShadowFrame& frame, void* stream) {
    namespace sk = sdf_shadow_kernel;
    if (!sk::params_valid(frame)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize pixels = static_cast<usize>(frame.width) * frame.height;

    kernel::cuda::DeviceBuffer<compute::SdfObject> objects;
    kernel::cuda::DeviceBuffer<math::Vec4> positions;
    kernel::cuda::DeviceBuffer<math::Vec3> normals;
    kernel::cuda::DeviceBuffer<f32> shadow;
    bool ok = objects.allocate(frame.objectCount) && objects.upload(frame.objects, frame.objectCount, cudaStream) &&
              positions.allocate(pixels) && positions.upload(frame.worldPositions, pixels, cudaStream) &&
              shadow.allocate(pixels);
    if (ok && frame.normals != nullptr) {
        ok = normals.allocate(pixels) && normals.upload(frame.normals, pixels, cudaStream);
    }
    if (!ok) {
        return false;
    }

    SdfShadowFrame deviceFrame = frame;
    deviceFrame.objects = objects.data();
    deviceFrame.worldPositions = positions.data();
    deviceFrame.normals = frame.normals != nullptr ? normals.data() : nullptr;
    deviceFrame.outShadow = shadow.data();

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<sk::Kernel, sk::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, sk::make_launch(frame), sk::Kernel{}, sk::make_params(deviceFrame),
                        options)
             .ok;
    ok = ok && shadow.download(frame.outShadow, pixels, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

} // namespace fuse::renderer
