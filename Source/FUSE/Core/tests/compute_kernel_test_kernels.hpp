#pragma once

// Single-source test kernels shared by the host gate (test_compute_kernel_gates.cpp, CPU backends)
// and the CUDA device-compile gate (cuda/test_compute_kernel_device.cu).

#include <fuse/compute_kernel/atomics.hpp>
#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

#include <cmath>

namespace fuse::kernel_test {

using kernel::LaunchIndex;
using kernel::Span;
using kernel::WorkgroupContext;

FUSE_HOST_DEVICE inline u32 hash_u32(u32 x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// ---- item kernels -------------------------------------------------------------------------------

struct HashParams {
    Span<u32> out;
    u32 seed = 0;
};

/// out[linear] = hash(global xyz, seed): integer, bit-exact on every backend.
struct HashKernel {
    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const HashParams& p) const {
        p.out[idx.linear] = hash_u32(idx.global.x ^ hash_u32(idx.global.y ^ hash_u32(idx.global.z + p.seed)));
    }
};

struct WaveParams {
    Span<f32> out;
    f32 frequency = 1.f;
};

/// Float work with transcendental calls — bit-exact between CPU backends, tolerance vs a GPU.
struct WaveKernel {
    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const WaveParams& p) const {
        const f32 u = static_cast<f32>(idx.global.x) / static_cast<f32>(idx.grid.x);
        const f32 v = static_cast<f32>(idx.global.y) / static_cast<f32>(idx.grid.y);
        p.out[idx.linear] = std::sin(u * p.frequency) * std::cos(v * p.frequency) + std::sqrt(u * v + 1.f);
    }
};

struct OrderParams {
    Span<u32> visit_order; ///< visit_order[n] = linear index of the n-th invocation.
    u32* cursor = nullptr;
};

/// Records invocation order. Serial backends only (non-atomic cursor) — the CpuReference order test.
struct OrderKernel {
    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const OrderParams& p) const {
        p.visit_order[(*p.cursor)++] = idx.linear;
    }
};

// ---- workgroup kernels --------------------------------------------------------------------------

inline constexpr u32 kHistogramBins = 256;

struct HistogramParams {
    Span<const u32> values;    ///< Each value's low 8 bits select the bin.
    Span<u32> group_histograms; ///< [group][bin] per-workgroup histogram.
    Span<u32> histogram;        ///< [bin] global histogram (atomic accumulation across workgroups).
};

/// Tiled histogram: phase 0 clears scratch bins, phase 1 accumulates the tile with scratch atomics,
/// phase 2 publishes the tile histogram and adds it to the global one. Workgroup must be 256 x 1 x 1.
struct TiledHistogramKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = kHistogramBins;
    static constexpr u32 kPhases = 3;

    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const WorkgroupContext<u32>& wg,
                                     const HistogramParams& p) const {
        const u32 t = idx.local_linear;
        switch (wg.phase) {
        case 0:
            wg.scratch[t] = 0u;
            break;
        case 1:
            if (idx.active) {
                kernel::scratch_atomic_add(&wg.scratch[p.values[idx.linear] & 0xffu], 1u);
            }
            break;
        default: {
            const u32 group = idx.group.x;
            p.group_histograms[group * kHistogramBins + t] = wg.scratch[t];
            if (wg.scratch[t] != 0u) {
                kernel::global_atomic_add(&p.histogram[t], wg.scratch[t]);
            }
            break;
        }
        }
    }
};

inline constexpr u32 kScanGroupSize = 256;
inline constexpr u32 kScanSteps = 8; // log2(kScanGroupSize)

struct BlockScanParams {
    Span<const u32> input;
    Span<u32> output;     ///< Exclusive scan within each workgroup.
    Span<u32> block_sums; ///< [group] = sum of the group's inputs (null span = not written).
};

/// Workgroup exclusive prefix scan (Hillis-Steele, ping-pong scratch halves, one step per phase):
/// phase 0 loads, phases 1..8 run the log-steps, phase 9 stores. Workgroup must be 256 x 1 x 1.
struct BlockScanKernel {
    using Scratch = u32;
    static constexpr u32 kScratchCount = 2 * kScanGroupSize;
    static constexpr u32 kPhases = kScanSteps + 2;

    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const WorkgroupContext<u32>& wg,
                                     const BlockScanParams& p) const {
        const u32 t = idx.local_linear;
        const u32 phase = wg.phase;
        if (phase == 0u) {
            wg.scratch[t] = idx.active ? p.input[idx.linear] : 0u;
            return;
        }
        if (phase <= kScanSteps) {
            const u32 offset = 1u << (phase - 1u);
            const u32* src = wg.scratch + ((phase - 1u) & 1u) * kScanGroupSize;
            u32* dst = wg.scratch + (phase & 1u) * kScanGroupSize;
            dst[t] = src[t] + (t >= offset ? src[t - offset] : 0u);
            return;
        }
        const u32* inclusive = wg.scratch + (kScanSteps & 1u) * kScanGroupSize;
        if (idx.active) {
            p.output[idx.linear] = inclusive[t] - p.input[idx.linear];
        }
        if (t == kScanGroupSize - 1u && !p.block_sums.empty()) {
            p.block_sums[idx.group.x] = inclusive[t];
        }
    }
};

struct AddOffsetsParams {
    Span<u32> data;
    Span<const u32> offsets; ///< Exclusive scan of block sums, one per scan workgroup.
};

/// Adds each scan block's offset to its elements (item kernel, grid = element count).
struct AddOffsetsKernel {
    FUSE_HOST_DEVICE void operator()(const LaunchIndex& idx, const AddOffsetsParams& p) const {
        p.data[idx.linear] += p.offsets[idx.linear / kScanGroupSize];
    }
};

} // namespace fuse::kernel_test
