// CUDA side of the CUDA <-> Vulkan interop gates (test_cuda_vk_interop_gates.cpp). Every kernel works on
// memory imported from Vulkan (cudaImportExternalMemory): the SharedTimeline peer lane, full-buffer pattern
// writes / checks, and the surface write into an imported Vulkan image. The per-word logic is the shared
// FUSE_HOST_DEVICE code in cuda_vk_interop_gate_common.hpp (the CPU-lane gate runs the same functions).
// Launchers return the cudaError_t of the launch as int (0 = cudaSuccess) and never synchronise.

#include "../cuda_vk_interop_gate_common.hpp"

#include <cuda_runtime.h>

#include <cstdint>

namespace {

using fuse::u32;
using fuse::u8;
namespace gate = fuse::cuda_vk_gate;

__global__ void peerLaneKernel(u32* words, u32 frame, u32 count, u32* log) {
    const u32 i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i == 0u) {
        log[frame * gate::kLogWordsPerFrame] = words[gate::kVulkanFrameWord];
        words[gate::kPeerFrameWord] = frame;
    }
    if (i >= count) {
        return;
    }
    const u32 bad = gate::peerLaneStep(words, frame, i);
    if (bad != 0u) {
        atomicAdd(&log[frame * gate::kLogWordsPerFrame + 1u], bad);
    }
}

__global__ void writePatternKernel(u32* data, u32 count, u32 frame, u32 lane) {
    const u32 stride = blockDim.x * gridDim.x;
    for (u32 i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += stride) {
        data[i] = gate::laneValue(frame, lane, i);
    }
}

__global__ void countMismatchesKernel(const u32* data, u32 count, u32 frame, u32 lane, unsigned long long* mismatches,
                                      u32* firstBad) {
    const u32 stride = blockDim.x * gridDim.x;
    unsigned long long local = 0;
    for (u32 i = blockIdx.x * blockDim.x + threadIdx.x; i < count; i += stride) {
        if (data[i] != gate::laneValue(frame, lane, i)) {
            ++local;
            atomicMin(firstBad, i);
        }
    }
    if (local != 0u) {
        atomicAdd(mismatches, local);
    }
}

__global__ void surfacePatternKernel(cudaSurfaceObject_t surface, u32 width, u32 height, u32 frame) {
    const u32 x = blockIdx.x * blockDim.x + threadIdx.x;
    const u32 y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) {
        return;
    }
    u8 rgba[4];
    gate::surfacePattern(frame, x, y, rgba);
    surf2Dwrite(make_uchar4(rgba[0], rgba[1], rgba[2], rgba[3]), surface, static_cast<int>(x * sizeof(uchar4)),
                static_cast<int>(y));
}

unsigned int gridFor(u32 count, u32 block) {
    const u32 blocks = (count + block - 1u) / block;
    return blocks == 0u ? 1u : blocks;
}

cudaStream_t asStream(void* stream) {
    return static_cast<cudaStream_t>(stream);
}

} // namespace

extern "C" int fuse_cvk_peer_lane(void* words, u32 frame, u32 count, u32* log, void* stream) {
    peerLaneKernel<<<gridFor(count, 256u), 256, 0, asStream(stream)>>>(static_cast<u32*>(words), frame, count, log);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int fuse_cvk_write_pattern(void* data, u32 count, u32 frame, u32 lane, void* stream) {
    const unsigned int grid = gridFor(count, 256u) > 4096u ? 4096u : gridFor(count, 256u);
    writePatternKernel<<<grid, 256, 0, asStream(stream)>>>(static_cast<u32*>(data), count, frame, lane);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int fuse_cvk_count_mismatches(const void* data, u32 count, u32 frame, u32 lane,
                                         unsigned long long* mismatches, u32* firstBad, void* stream) {
    const unsigned int grid = gridFor(count, 256u) > 4096u ? 4096u : gridFor(count, 256u);
    countMismatchesKernel<<<grid, 256, 0, asStream(stream)>>>(static_cast<const u32*>(data), count, frame, lane,
                                                              mismatches, firstBad);
    return static_cast<int>(cudaGetLastError());
}

extern "C" int fuse_cvk_surface_pattern(void* surfaceObject, u32 width, u32 height, u32 frame, void* stream) {
    const cudaSurfaceObject_t surface =
        static_cast<cudaSurfaceObject_t>(reinterpret_cast<std::uintptr_t>(surfaceObject));
    const dim3 block(16, 16);
    const dim3 grid(gridFor(width, 16u), gridFor(height, 16u));
    surfacePatternKernel<<<grid, block, 0, asStream(stream)>>>(surface, width, height, frame);
    return static_cast<int>(cudaGetLastError());
}
