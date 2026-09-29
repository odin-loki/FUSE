#pragma once

// Device benchmark of the single-source "svo_ray_cast" kernel (fuse/scene/svo_ray_kernel.hpp) for the
// B3 row "SVO ray cast for 1M rays < 10 ms on CUDA (RTX 3090) — verified with CUDA event timing".
// The SVO's linearised ray-walk layout (SVO::rayLayout) and the rays are uploaded once into resident
// device buffers; each timed launch is bracketed by CUDA events on one stream, so the kernel time
// excludes allocation and PCIe copies.
// Without a CUDA build or device the call returns false and leaves `timing` untouched.

#include <fuse/scene/svo_ray_kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::scene {

class SVO;

struct SvoRayDeviceTiming {
    f32 layout_ms = 0.f;      ///< SVO::rayLayout on the host (steady clock)
    usize layout_bytes = 0;   ///< packed nodes + bricks + payload words uploaded
    f32 upload_ms = 0.f;      ///< packed layout + rays, host -> device (CUDA events)
    f32 kernel_ms_min = 0.f;  ///< best launch over `iterations` (CUDA events, resident buffers)
    f32 kernel_ms_avg = 0.f;  ///< mean launch over `iterations`
    f32 download_ms = 0.f;    ///< hit records, device -> host (CUDA events)
    u32 iterations = 0;
    u32 threads_per_block = 0;
    s32 registers_per_thread = -1; ///< cudaFuncGetAttributes
    s32 local_bytes_per_thread = -1;
    s32 blocks_per_sm = -1;        ///< cudaOccupancyMaxActiveBlocksPerMultiprocessor
    f32 theoretical_occupancy = 0.f; ///< blocks_per_sm * threads / max threads per SM
};

/// Uploads `svo` + `rays` once, runs `iterations` (>= 1) launches of the kernel on the resident buffers,
/// downloads the last launch's hit records into `hits` and fills `timing`. False without a device.
bool benchmarkSvoRayCastCuda(const SVO& svo, const SvoRay* rays, SvoRayHit* hits, u32 count, u32 iterations,
                             SvoRayDeviceTiming& timing);

} // namespace fuse::scene
