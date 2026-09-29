// CUDA backend of linear blend skinning: the __global__ trampoline from cuda_launch.cuh runs the same
// FUSE_HOST_DEVICE body (fuse/animation/skinning_kernel.hpp) the CPU backends run. This TU only stages
// the mesh, weights and bone palette in device memory and reads the skinned vertices back.

#include <fuse/animation/skinning.hpp>
#include <fuse/animation/skinning_kernel.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <vector>

namespace fuse::animation {

bool launchSkinningCuda(const skinning_kernel::Params& params, void* stream) {
    namespace sk = skinning_kernel;
    if (!sk::params_valid(params)) {
        return false;
    }
    const cudaStream_t cudaStream = static_cast<cudaStream_t>(stream);
    const usize vertices = params.rest_positions.size;
    const usize normals = params.rest_normals.size;

    kernel::cuda::DeviceBuffer<vec3> restPositions;
    kernel::cuda::DeviceBuffer<vec3> restNormals;
    kernel::cuda::DeviceBuffer<SkinningWeights> weights;
    kernel::cuda::DeviceBuffer<mat4> bones;
    kernel::cuda::DeviceBuffer<vec3> outPositions;
    kernel::cuda::DeviceBuffer<vec3> outNormals;
    bool ok = restPositions.allocate(vertices) && restNormals.allocate(normals) && weights.allocate(vertices) &&
              bones.allocate(params.bones.size) && outPositions.allocate(vertices) && outNormals.allocate(normals);
    ok = ok && restPositions.upload(params.rest_positions.data, vertices, cudaStream) &&
         restNormals.upload(params.rest_normals.data, normals, cudaStream) &&
         weights.upload(params.weights.data, vertices, cudaStream) &&
         bones.upload(params.bones.data, params.bones.size, cudaStream);
    if (!ok) {
        return false;
    }

    sk::Params device = params;
    device.rest_positions.data = restPositions.data();
    device.rest_normals.data = restNormals.data();
    device.weights.data = weights.data();
    device.bones.data = bones.data();
    device.out_positions.data = outPositions.data();
    device.out_normals.data = outNormals.data();

    kernel::LaunchOptions options{};
    options.cuda = &kernel::cuda::entry<sk::Kernel, sk::Params>;
    options.stream = stream;
    options.allow_fallback = false;
    options.synchronize = true;
    ok = kernel::launch(kernel::Backend::Cuda, sk::make_launch(params), sk::Kernel{}, device, options).ok;
    ok = ok && outPositions.download(params.out_positions.data, vertices, cudaStream) &&
         outNormals.download(params.out_normals.data, normals, cudaStream);
    return ok && cudaStreamSynchronize(cudaStream) == cudaSuccess;
}

namespace {

/// cudaEvent pair that is always destroyed.
struct EventPair {
    cudaEvent_t start = nullptr;
    cudaEvent_t stop = nullptr;
    bool ok = false;
    EventPair() { ok = cudaEventCreate(&start) == cudaSuccess && cudaEventCreate(&stop) == cudaSuccess; }
    ~EventPair() {
        if (start != nullptr) {
            cudaEventDestroy(start);
        }
        if (stop != nullptr) {
            cudaEventDestroy(stop);
        }
    }
    EventPair(const EventPair&) = delete;
    EventPair& operator=(const EventPair&) = delete;
    f32 elapsed() const {
        float ms = 0.f;
        return cudaEventElapsedTime(&ms, start, stop) == cudaSuccess ? ms : -1.f;
    }
};

f32 medianOf(std::vector<f32>& values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.f : values[values.size() / 2u];
}

} // namespace

bool timeSkinningCudaResident(const skinning_kernel::Params& params, u32 iterations, SkinningCudaTiming& timing) {
    namespace sk = skinning_kernel;
    if (!sk::params_valid(params) || iterations == 0u) {
        timing.reason = "invalid skinning input";
        return false;
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
        timing.reason = "cudaStreamCreate failed";
        return false;
    }
    const usize vertices = params.rest_positions.size;
    const usize normals = params.rest_normals.size;
    bool ok = true;
    {
        kernel::cuda::DeviceBuffer<vec3> restPositions;
        kernel::cuda::DeviceBuffer<vec3> restNormals;
        kernel::cuda::DeviceBuffer<SkinningWeights> weights;
        kernel::cuda::DeviceBuffer<mat4> bones;
        kernel::cuda::DeviceBuffer<vec3> outPositions;
        kernel::cuda::DeviceBuffer<vec3> outNormals;
        EventPair upload;
        EventPair download;
        EventPair batch;
        ok = upload.ok && download.ok && batch.ok && restPositions.allocate(vertices) && restNormals.allocate(normals) &&
             weights.allocate(vertices) && bones.allocate(params.bones.size) && outPositions.allocate(vertices) &&
             outNormals.allocate(normals);
        ok = ok && cudaEventRecord(upload.start, stream) == cudaSuccess &&
             restPositions.upload(params.rest_positions.data, vertices, stream) &&
             restNormals.upload(params.rest_normals.data, normals, stream) &&
             weights.upload(params.weights.data, vertices, stream) &&
             bones.upload(params.bones.data, params.bones.size, stream) &&
             cudaEventRecord(upload.stop, stream) == cudaSuccess;

        sk::Params device = params;
        device.rest_positions.data = restPositions.data();
        device.rest_normals.data = restNormals.data();
        device.weights.data = weights.data();
        device.bones.data = bones.data();
        device.out_positions.data = outPositions.data();
        device.out_normals.data = outNormals.data();

        kernel::LaunchOptions options{};
        options.cuda = &kernel::cuda::entry<sk::Kernel, sk::Params>;
        options.stream = stream;
        options.allow_fallback = false;
        options.synchronize = false;
        const kernel::KernelLaunch launchDesc = sk::make_launch(params);
        const auto launchOnce = [&] {
            return kernel::launch(kernel::Backend::Cuda, launchDesc, sk::Kernel{}, device, options).ok;
        };

        ok = ok && launchOnce() && cudaStreamSynchronize(stream) == cudaSuccess; // warm-up (module load, caches)
        timing.upload_ms = ok ? upload.elapsed() : 0.f;

        // Single launches, one event pair each.
        std::vector<f32> single;
        single.reserve(iterations);
        for (u32 i = 0; ok && i < iterations; ++i) {
            EventPair pair;
            ok = pair.ok && cudaEventRecord(pair.start, stream) == cudaSuccess && launchOnce() &&
                 cudaEventRecord(pair.stop, stream) == cudaSuccess && cudaEventSynchronize(pair.stop) == cudaSuccess;
            if (ok) {
                single.push_back(pair.elapsed());
            }
        }
        // Back-to-back batch: no host gaps between kernels on the GPU timeline.
        ok = ok && cudaEventRecord(batch.start, stream) == cudaSuccess;
        for (u32 i = 0; ok && i < iterations; ++i) {
            ok = launchOnce();
        }
        ok = ok && cudaEventRecord(batch.stop, stream) == cudaSuccess && cudaEventSynchronize(batch.stop) == cudaSuccess;
        timing.batch_ms_per_launch = ok ? batch.elapsed() / static_cast<f32>(iterations) : 0.f;
        // Per-frame: palette upload (the Animator's bone buffer) + launch.
        std::vector<f32> frames;
        frames.reserve(iterations);
        for (u32 i = 0; ok && i < iterations; ++i) {
            EventPair pair;
            ok = pair.ok && cudaEventRecord(pair.start, stream) == cudaSuccess &&
                 bones.upload(params.bones.data, params.bones.size, stream) && launchOnce() &&
                 cudaEventRecord(pair.stop, stream) == cudaSuccess && cudaEventSynchronize(pair.stop) == cudaSuccess;
            if (ok) {
                frames.push_back(pair.elapsed());
            }
        }
        ok = ok && cudaEventRecord(download.start, stream) == cudaSuccess &&
             outPositions.download(params.out_positions.data, vertices, stream) &&
             outNormals.download(params.out_normals.data, normals, stream) &&
             cudaEventRecord(download.stop, stream) == cudaSuccess && cudaStreamSynchronize(stream) == cudaSuccess;
        if (ok) {
            timing.download_ms = download.elapsed();
            std::vector<f32> sorted = single;
            timing.kernel_ms_median = medianOf(sorted);
            timing.kernel_ms_min = sorted.front();
            timing.kernel_ms_max = sorted.back();
            timing.frame_ms_median = medianOf(frames);
            timing.ran = true;
        } else {
            timing.reason = cudaGetErrorString(cudaGetLastError());
        }
    }
    cudaStreamDestroy(stream);
    return ok;
}

} // namespace fuse::animation
