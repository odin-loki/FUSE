#pragma once

// Shared definitions of the CUDA <-> Vulkan interop gates (test_cuda_vk_interop_gates.cpp, the CUDA
// kernels in cuda/cuda_vk_interop_gate_kernels.cu and the Vulkan compute shader
// shaders/cuda_vk_timeline_lane.comp, which carries a GLSL twin of laneValue()).
//
// SharedTimeline protocol (one timeline semaphore T, frames f = 1..F):
//   Vulkan lane  waits T >= 2f-2, checks every data word == laneValue(f-1, peer), writes laneValue(f,
//                vulkan), logs {peer frame word it saw, mismatches}, signals T = 2f-1.
//   Peer lane    (CUDA kernel on the 3090, or the host in the CPU-lane gate) waits T >= 2f-1, checks every
//                word == laneValue(f, vulkan), writes laneValue(f, peer), logs {vulkan frame word, mismatches},
//                signals T = 2f.
// Any overlap of the two lanes shows up as a mismatch or a wrong frame word in some frame's log.

#include <fuse/types.hpp>

namespace fuse::cuda_vk_gate {

/// Words before the data array: [0] frame the Vulkan lane last wrote, [1] frame the peer lane last wrote.
inline constexpr u32 kHeaderWords = 64;
inline constexpr u32 kVulkanFrameWord = 0;
inline constexpr u32 kPeerFrameWord = 1;
inline constexpr u32 kLaneVulkan = 0;
inline constexpr u32 kLanePeer = 1;
inline constexpr u32 kLanePatternA = 2;
inline constexpr u32 kLanePatternB = 3;
/// Log entries per frame: {frame word of the other lane as seen, mismatching words}.
inline constexpr u32 kLogWordsPerFrame = 2;
/// Compute workgroup size of cuda_vk_timeline_lane.comp.
inline constexpr u32 kLaneWorkgroup = 256;

/// Value lane `lane` writes to data word `i` in frame `frame`; frame 0 is the zero fill. Never 0 otherwise.
/// Must stay identical to laneValue() in shaders/cuda_vk_timeline_lane.comp.
FUSE_HOST_DEVICE inline u32 laneValue(u32 frame, u32 lane, u32 i) {
    if (frame == 0u) {
        return 0u;
    }
    u32 x = (frame * 0x9E3779B1u) ^ (i * 0x85EBCA77u) ^ (lane * 0xC2B2AE3Du);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x | 1u;
}

/// Peer-lane step for data word `i` of frame `frame` (the CUDA kernel runs one per thread; the CPU lane
/// loops it). Returns 1 when the word did not hold the Vulkan lane's value for this frame.
FUSE_HOST_DEVICE inline u32 peerLaneStep(u32* words, u32 frame, u32 i) {
    u32* data = words + kHeaderWords;
    const u32 mismatch = data[i] != laneValue(frame, kLaneVulkan, i) ? 1u : 0u;
    data[i] = laneValue(frame, kLanePeer, i);
    return mismatch;
}

/// RGBA8 texel the CUDA surface-write gate writes at (x, y) in frame `frame`.
FUSE_HOST_DEVICE inline void surfacePattern(u32 frame, u32 x, u32 y, u8 rgba[4]) {
    rgba[0] = static_cast<u8>((x * 7u + frame * 13u) & 0xffu);
    rgba[1] = static_cast<u8>((y * 5u + frame * 29u) & 0xffu);
    rgba[2] = static_cast<u8>((x ^ y ^ (frame * 3u)) & 0xffu);
    rgba[3] = 255u;
}

} // namespace fuse::cuda_vk_gate
