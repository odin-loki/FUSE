// CUDA half of the B5 per-pass frame benchmark (fuse_b5_frame_bench; see test_b5_frame_bench.cpp). Each pass
// stages its inputs on the device ONCE, then runs the single-source kernel bodies the engine runs — through the
// same kernel::launch / cuda::entry trampolines as the modules' own .cu wrappers — on resident buffers:
//
//   sdf_ray_march           fuse/compute/ray_march_kernel.hpp        (100 objects, 128 steps)
//   sdf_shadows             fuse/renderer/shadow/sdf_shadow_kernel.hpp (occluders = the same 100 objects)
//   screen_space_ao (+blur) fuse/ssfx/hbao_kernel.hpp + compute::screen_space_kernels::ao_blur
//   screen_space_reflections fuse/ssfx/ssr_kernel.hpp
//
// Timing: CUDA events around the pass on its stream (asynchronous launches) per iteration. Profiler cross-check:
// the same pass again with synchronous launches, comparing the sum of the kernels' kernel::LaunchResult durations
// (what record_launch and the FUSE_PROFILE_SCOPE of each launch record — the profiler's pass breakdown) with the
// CUDA-event time of those iterations.

#include "../b5_frame_bench.hpp"

#include <fuse/compute/ray_march_kernel.hpp>
#include <fuse/compute/screen_space_kernels.hpp>
#include <fuse/compute_kernel/cuda_launch.cuh>
#include <fuse/compute_kernel/launch.hpp>
#include <fuse/renderer/shadow/sdf_shadow_kernel.hpp>

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>

namespace b5bench {

namespace {

namespace kernel = fuse::kernel;
namespace compute = fuse::compute;
namespace ssk = fuse::compute::screen_space_kernels;
namespace sk = fuse::renderer::sdf_shadow_kernel;
namespace rm = fuse::compute::ray_march_kernel;
namespace hbao = fuse::ssfx::hbao_kernel;
namespace ssr = fuse::ssfx::ssr_kernel;
using fuse::u64;

/// A stream + event pair owned by one pass.
struct StreamEvents {
    cudaStream_t stream = nullptr;
    cudaEvent_t start = nullptr;
    cudaEvent_t stop = nullptr;
    bool ok = false;

    StreamEvents() {
        ok = cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) == cudaSuccess &&
             cudaEventCreate(&start) == cudaSuccess && cudaEventCreate(&stop) == cudaSuccess;
    }
    ~StreamEvents() {
        if (stream != nullptr) {
            cudaStreamSynchronize(stream);
        }
        if (start != nullptr) {
            cudaEventDestroy(start);
        }
        if (stop != nullptr) {
            cudaEventDestroy(stop);
        }
        if (stream != nullptr) {
            cudaStreamDestroy(stream);
        }
    }
    StreamEvents(const StreamEvents&) = delete;
    StreamEvents& operator=(const StreamEvents&) = delete;

    f64 elapsed() const {
        float ms = 0.f;
        return cudaEventElapsedTime(&ms, start, stop) == cudaSuccess ? static_cast<f64>(ms) : 0.0;
    }
};

kernel::LaunchOptions options(kernel::DeviceEntryFn entry, cudaStream_t stream, bool synchronize) {
    kernel::LaunchOptions o{};
    o.cuda = entry;
    o.stream = stream;
    o.allow_fallback = false;
    o.synchronize = synchronize;
    return o;
}

bool failPass(PassResult& out, const char* what) {
    const cudaError_t error = cudaGetLastError();
    out.failed = true;
    out.note = std::string(what) + ": " + (error != cudaSuccess ? cudaGetErrorString(error) : "launch failed");
    return false;
}

/// Warm-up, asynchronous CUDA-event loop, then the synchronous profiler loop. `enqueue(synchronize, profilerNs)`
/// enqueues the whole pass on `se.stream` and adds its launches' LaunchResult::duration_ns.
template <typename Enqueue>
bool timePass(const BenchConfig& cfg, StreamEvents& se, PassResult& out, Enqueue&& enqueue) {
    u64 ns = 0u;
    for (u32 i = 0; i < cfg.warmup; ++i) {
        if (!enqueue(false, ns)) {
            return failPass(out, "warm-up");
        }
    }
    if (cudaStreamSynchronize(se.stream) != cudaSuccess) {
        return failPass(out, "warm-up sync");
    }
    for (u32 i = 0; i < cfg.iterations; ++i) {
        const bool ok = cudaEventRecord(se.start, se.stream) == cudaSuccess && enqueue(false, ns) &&
                        cudaEventRecord(se.stop, se.stream) == cudaSuccess &&
                        cudaEventSynchronize(se.stop) == cudaSuccess;
        if (!ok) {
            return failPass(out, "timed iteration");
        }
        out.ms.push_back(se.elapsed());
    }
    f64 profiler = 0.0;
    f64 events = 0.0;
    for (u32 i = 0; i < cfg.iterations; ++i) {
        u64 launchNs = 0u;
        const bool ok = cudaEventRecord(se.start, se.stream) == cudaSuccess && enqueue(true, launchNs) &&
                        cudaEventRecord(se.stop, se.stream) == cudaSuccess &&
                        cudaEventSynchronize(se.stop) == cudaSuccess;
        if (!ok) {
            return failPass(out, "profiled iteration");
        }
        profiler += static_cast<f64>(launchNs) * 1e-6;
        events += se.elapsed();
    }
    if (cfg.iterations > 0u) {
        out.profilerMs = profiler / static_cast<f64>(cfg.iterations);
        out.profilerEventMs = events / static_cast<f64>(cfg.iterations);
    }
    out.ran = true;
    return true;
}

template <typename T>
bool stage(kernel::cuda::DeviceBuffer<T>& buffer, const T* host, usize count, cudaStream_t stream) {
    return buffer.allocate(count) && (host == nullptr || buffer.upload(host, count, stream));
}

compute::RayMarchParams marchScene(const CudaPassInputs& in) {
    const Frame& f = *in.frame;
    compute::RayMarchParams scene{};
    scene.width = f.width;
    scene.height = f.height;
    scene.cam_pos = f.view.position;
    scene.cam_forward = f.view.back * -1.f;
    scene.cam_right = f.view.right;
    scene.cam_up = f.view.up;
    scene.fov_rad = kFovYRadians;
    scene.max_steps = 128u;
    scene.min_dist = 1e-3f;
    scene.max_dist = kFarPlane;
    scene.objects = in.objects->data();
    scene.object_count = static_cast<u32>(in.objects->size());
    return scene;
}

void cameraOf(const CudaPassInputs& in, f32 (&proj)[16]) {
    for (u32 i = 0; i < 16u; ++i) {
        proj[i] = in.proj[i];
    }
}

} // namespace

std::string cudaDeviceDescription() {
    int device = 0;
    cudaDeviceProp props{};
    if (cudaGetDevice(&device) != cudaSuccess || cudaGetDeviceProperties(&props, device) != cudaSuccess) {
        return "unknown CUDA device";
    }
    // cudaDeviceProp::clockRate is gone in CUDA 13: name, architecture and SM count only.
    char text[320];
    std::snprintf(text, sizeof(text), "%s (sm_%d%d, %d SMs, %.1f GiB)", props.name, props.major, props.minor,
                  props.multiProcessorCount, static_cast<double>(props.totalGlobalMem) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

void cudaBenchRayMarch(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out) {
    const compute::RayMarchParams host = marchScene(in);
    const usize pixels = static_cast<usize>(host.width) * host.height;
    StreamEvents se;
    kernel::cuda::DeviceBuffer<compute::SdfObject> objects;
    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<fuse::math::Vec4> normals;
    if (!se.ok || !stage(objects, host.objects, host.object_count, se.stream) || !stage<f32>(depth, nullptr, pixels, se.stream) ||
        !stage<fuse::math::Vec4>(normals, nullptr, pixels, se.stream) || cudaStreamSynchronize(se.stream) != cudaSuccess) {
        failPass(out, "staging");
        return;
    }
    compute::RayMarchParams device = host;
    device.objects = objects.data();
    device.depth_surface = depth.data();
    device.output_surface = normals.data();
    const rm::Params params = rm::make_params(device);
    const kernel::KernelLaunch launch = rm::make_launch(device);
    const kernel::DeviceEntryFn entry = &kernel::cuda::entry<rm::Kernel, rm::Params>;
    const bool ok = timePass(cfg, se, out, [&](bool sync, u64& ns) {
        const kernel::LaunchResult r = kernel::launch(kernel::Backend::Cuda, launch, rm::Kernel{}, params,
                                                      options(entry, se.stream, sync));
        ns += r.duration_ns;
        return r.ok;
    });
    if (!ok) {
        return;
    }
    std::vector<f32> hits(pixels);
    if (depth.download(hits.data(), pixels, se.stream) && cudaStreamSynchronize(se.stream) == cudaSuccess) {
        usize hit = 0u;
        for (f32 t : hits) {
            hit += t >= 0.f ? 1u : 0u;
        }
        char note[128];
        std::snprintf(note, sizeof(note), "%.1f%% of pixel rays hit", 100.0 * static_cast<f64>(hit) / static_cast<f64>(pixels));
        out.note = note;
        if (hit == 0u) {
            out.failed = true;
            out.note += " (expected geometry)";
        }
    }
}

void cudaBenchSdfShadows(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out) {
    const Frame& f = *in.frame;
    const usize pixels = static_cast<usize>(f.width) * f.height;
    fuse::renderer::SdfShadowFrame host{};
    host.width = f.width;
    host.height = f.height;
    host.worldPositions = f.worldPos.data();
    host.normals = f.normals.data();
    host.objects = in.objects->data();
    host.objectCount = static_cast<u32>(in.objects->size());
    host.sceneMaxDistance = kFarPlane;
    host.lightDirection = in.sunDirection;
    host.normalBias = 0.02f;
    std::vector<f32> shadow(pixels, 1.f);
    host.outShadow = shadow.data();
    if (!sk::params_valid(host)) {
        out.failed = true;
        out.note = "invalid shadow frame";
        return;
    }
    StreamEvents se;
    kernel::cuda::DeviceBuffer<compute::SdfObject> objects;
    kernel::cuda::DeviceBuffer<fuse::math::Vec4> positions;
    kernel::cuda::DeviceBuffer<Vec3> normals;
    kernel::cuda::DeviceBuffer<f32> factors;
    if (!se.ok || !stage(objects, host.objects, host.objectCount, se.stream) ||
        !stage(positions, host.worldPositions, pixels, se.stream) || !stage(normals, host.normals, pixels, se.stream) ||
        !stage<f32>(factors, nullptr, pixels, se.stream) || cudaStreamSynchronize(se.stream) != cudaSuccess) {
        failPass(out, "staging");
        return;
    }
    fuse::renderer::SdfShadowFrame device = host;
    device.objects = objects.data();
    device.worldPositions = positions.data();
    device.normals = normals.data();
    device.outShadow = factors.data();
    const sk::Params params = sk::make_params(device);
    const kernel::KernelLaunch launch = sk::make_launch(device);
    const kernel::DeviceEntryFn entry = &kernel::cuda::entry<sk::Kernel, sk::Params>;
    const bool ok = timePass(cfg, se, out, [&](bool sync, u64& ns) {
        const kernel::LaunchResult r =
            kernel::launch(kernel::Backend::Cuda, launch, sk::Kernel{}, params, options(entry, se.stream, sync));
        ns += r.duration_ns;
        return r.ok;
    });
    if (!ok) {
        return;
    }
    if (factors.download(shadow.data(), pixels, se.stream) && cudaStreamSynchronize(se.stream) == cudaSuccess) {
        usize shadowed = 0u;
        bool finite = true;
        for (f32 s : shadow) {
            finite = finite && std::isfinite(s) && s >= 0.f && s <= 1.f;
            shadowed += s < 0.5f ? 1u : 0u;
        }
        char note[128];
        std::snprintf(note, sizeof(note), "%.1f%% of pixels in shadow", 100.0 * static_cast<f64>(shadowed) / static_cast<f64>(pixels));
        out.note = note;
        if (!finite) {
            out.failed = true;
            out.note += " (non-finite / out-of-range factors)";
        }
    }
}

void cudaBenchHbao(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out) {
    const Frame& f = *in.frame;
    const usize pixels = static_cast<usize>(f.width) * f.height;
    compute::SSAOParams params{};
    params.width = f.width;
    params.height = f.height;
    cameraOf(in, params.proj);
    params.radius = 1.f;
    params.directions = 8u;
    params.steps_per_dir = 4u;
    params.enable_blur = true;
    fuse::ssfx::SsfxGBufferView hostView{};
    if (!ssk::make_view(params.proj, f.width, f.height, f.viewDepth.data(), f.viewNormals.data(), hostView)) {
        out.failed = true;
        out.note = "invalid SSAO view";
        return;
    }
    StreamEvents se;
    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<Vec3> normals;
    kernel::cuda::DeviceBuffer<f32> raw;
    kernel::cuda::DeviceBuffer<f32> ao;
    if (!se.ok || !stage(depth, f.viewDepth.data(), pixels, se.stream) ||
        !stage(normals, f.viewNormals.data(), pixels, se.stream) || !stage<f32>(raw, nullptr, pixels, se.stream) ||
        !stage<f32>(ao, nullptr, pixels, se.stream) || cudaStreamSynchronize(se.stream) != cudaSuccess) {
        failPass(out, "staging");
        return;
    }
    fuse::ssfx::SsfxGBufferView view = hostView;
    view.depth = depth.data();
    view.normals = normals.data();
    const hbao::Params aoParams = hbao::make_params(view, ssk::to_hbao(params), raw.data());
    const ssk::ao_blur::Params blurParams{view, ssk::ao_blur::thresholds(params), raw.data(), ao.data()};
    const kernel::KernelLaunch aoLaunch = hbao::make_launch(view);
    const kernel::KernelLaunch blurLaunch = ssk::ao_blur::make_launch(view);
    const kernel::DeviceEntryFn aoEntry = &kernel::cuda::entry<hbao::Kernel, hbao::Params>;
    const kernel::DeviceEntryFn blurEntry = &kernel::cuda::entry<ssk::ao_blur::Kernel, ssk::ao_blur::Params>;
    const bool ok = timePass(cfg, se, out, [&](bool sync, u64& ns) {
        const kernel::LaunchResult a =
            kernel::launch(kernel::Backend::Cuda, aoLaunch, hbao::Kernel{}, aoParams, options(aoEntry, se.stream, sync));
        const kernel::LaunchResult b = kernel::launch(kernel::Backend::Cuda, blurLaunch, ssk::ao_blur::Kernel{},
                                                      blurParams, options(blurEntry, se.stream, sync));
        ns += a.duration_ns + b.duration_ns;
        return a.ok && b.ok;
    });
    if (!ok) {
        return;
    }
    std::vector<f32> result(pixels);
    if (ao.download(result.data(), pixels, se.stream) && cudaStreamSynchronize(se.stream) == cudaSuccess) {
        f64 sum = 0.0;
        bool finite = true;
        for (f32 v : result) {
            finite = finite && std::isfinite(v) && v >= 0.f && v <= 1.f + 1e-4f;
            sum += v;
        }
        char note[128];
        std::snprintf(note, sizeof(note), "mean visibility %.3f (screen_space_ao + screen_space_ao_blur)",
                      sum / static_cast<f64>(pixels));
        out.note = note;
        if (!finite) {
            out.failed = true;
            out.note += " (values outside [0, 1])";
        }
    }
}

void cudaBenchSsr(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out) {
    const Frame& f = *in.frame;
    const usize pixels = static_cast<usize>(f.width) * f.height;
    compute::SSRParams params{};
    params.width = f.width;
    params.height = f.height;
    cameraOf(in, params.proj);
    params.max_steps = 64u;
    params.max_distance = 20.f;
    fuse::ssfx::SsfxGBufferView hostView{};
    if (!ssk::make_view(params.proj, f.width, f.height, f.viewDepth.data(), f.viewNormals.data(), hostView)) {
        out.failed = true;
        out.note = "invalid SSR view";
        return;
    }
    StreamEvents se;
    kernel::cuda::DeviceBuffer<f32> depth;
    kernel::cuda::DeviceBuffer<Vec3> normals;
    kernel::cuda::DeviceBuffer<Vec3> color;
    kernel::cuda::DeviceBuffer<f32> roughness;
    kernel::cuda::DeviceBuffer<fuse::math::Vec4> reflections;
    if (!se.ok || !stage(depth, f.viewDepth.data(), pixels, se.stream) ||
        !stage(normals, f.viewNormals.data(), pixels, se.stream) || !stage(color, f.sceneColor.data(), pixels, se.stream) ||
        !stage(roughness, f.roughness.data(), pixels, se.stream) ||
        !stage<fuse::math::Vec4>(reflections, nullptr, pixels, se.stream) || cudaStreamSynchronize(se.stream) != cudaSuccess) {
        failPass(out, "staging");
        return;
    }
    fuse::ssfx::SsfxGBufferView view = hostView;
    view.depth = depth.data();
    view.normals = normals.data();
    const ssr::Params kp = ssk::make_ssr_params(view, params, color.data(), roughness.data(), reflections.data());
    const kernel::KernelLaunch launch = ssr::make_launch(view);
    const kernel::DeviceEntryFn entry = &kernel::cuda::entry<ssr::Kernel, ssr::Params>;
    const bool ok = timePass(cfg, se, out, [&](bool sync, u64& ns) {
        const kernel::LaunchResult r =
            kernel::launch(kernel::Backend::Cuda, launch, ssr::Kernel{}, kp, options(entry, se.stream, sync));
        ns += r.duration_ns;
        return r.ok;
    });
    if (!ok) {
        return;
    }
    std::vector<fuse::math::Vec4> result(pixels);
    if (reflections.download(result.data(), pixels, se.stream) && cudaStreamSynchronize(se.stream) == cudaSuccess) {
        usize hits = 0u;
        bool finite = true;
        for (const fuse::math::Vec4& v : result) {
            finite = finite && std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
            hits += v.w > 0.f ? 1u : 0u;
        }
        char note[128];
        std::snprintf(note, sizeof(note), "%.1f%% of pixels with a reflection hit", 100.0 * static_cast<f64>(hits) / static_cast<f64>(pixels));
        out.note = note;
        if (!finite) {
            out.failed = true;
            out.note += " (non-finite output)";
        }
    }
}

} // namespace b5bench
