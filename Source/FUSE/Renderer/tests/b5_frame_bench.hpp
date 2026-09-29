#pragma once

// Shared declarations of the B5 per-pass frame benchmark (fuse_b5_frame_bench): test_b5_frame_bench.cpp (driver,
// DDGI / clustered through their device classes, report), cuda/b5_frame_bench_cuda.cu (resident CUDA passes timed
// with CUDA events; FUSE_B5_FRAME_BENCH_CUDA builds only) and b5_frame_bench_vk.cpp (Vulkan passes timed with
// GpuProfiler timestamp queries).

#include "b5_bench_scene.hpp"

#include <fuse/compute/ray_march.hpp>
#include <fuse/types.hpp>

#include <string>
#include <vector>

namespace b5bench {

using fuse::f64;

struct BenchConfig {
    u32 width = 1920;
    u32 height = 1080;
    u32 iterations = 50;
    u32 warmup = 5;
    u32 draws = 1000;  ///< G-buffer draws ("G-buffer pass (1000 objects)")
    u32 lights = 1000; ///< clustered point lights
    bool cuda = true;
    bool vulkan = true;
};

/// One row of the report: a pass measured on the device (or why it was not).
struct PassResult {
    std::string key;   ///< stable id (render-graph / kernel pass name family)
    std::string label; ///< report text, with the workload
    std::string path;  ///< "CUDA events", "Vulkan timestamps" or "-"
    f64 budgetMs = 0.0;
    bool ran = false;
    bool failed = false; ///< ran into an error (as opposed to "not available here")
    std::string note;    ///< why it did not run / the error / what it covers
    std::vector<f64> ms; ///< per-iteration GPU time of the pass (asynchronous launches)
    /// Profiler cross-check (CUDA passes): the same pass with synchronous launches — the mean of what the engine's
    /// profiler records for its kernels (kernel::LaunchRecord::duration_ns == the FUSE_PROFILE_SCOPE span) and
    /// the mean CUDA-event time of those same iterations. < 0 = not applicable.
    f64 profilerMs = -1.0;
    f64 profilerEventMs = -1.0;
};

/// Host inputs shared by the CUDA passes (all built once from the benchmark frame).
struct CudaPassInputs {
    const Frame* frame = nullptr;
    const std::vector<fuse::compute::SdfObject>* objects = nullptr;
    Vec3 sunDirection{0.4f, 1.f, 0.3f};
    f32 proj[16]{}; ///< column-major perspective (Vulkan depth range), the frame camera
};

#if defined(FUSE_B5_FRAME_BENCH_CUDA)
/// cuda/b5_frame_bench_cuda.cu
std::string cudaDeviceDescription();
void cudaBenchRayMarch(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out);
void cudaBenchSdfShadows(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out);
void cudaBenchHbao(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out);
void cudaBenchSsr(const CudaPassInputs& in, const BenchConfig& cfg, PassResult& out);
#endif

/// b5_frame_bench_vk.cpp: the G-buffer raster pass, TAA (TAAU at 1x) and the GPU post stack at cfg.width x
/// cfg.height, timed with GpuProfiler zones. Fills `gbuffer`, `taa`, `post`; returns false when there is no
/// Vulkan device (the rows carry the reason). `deviceName` receives the Vulkan device.
bool vulkanBench(const BenchConfig& cfg, const Frame& frame, PassResult& gbuffer, PassResult& taa, PassResult& post,
                 std::string& deviceName);

} // namespace b5bench
