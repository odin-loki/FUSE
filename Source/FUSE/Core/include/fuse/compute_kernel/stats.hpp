#pragma once

// Per-kernel launch statistics registry (host only). Every kernel::launch() records one LaunchRecord;
// records aggregate per kernel name into KernelStats, queryable by tests, profiling tools and demos.
// Recording is heap-free (fixed-capacity table, names stored by pointer) and thread-safe.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/types.hpp>

namespace fuse::kernel {

/// One launch as executed (after backend resolution / fallback).
struct LaunchRecord {
    const char* name = nullptr;
    Backend requested = Backend::Auto;
    Backend backend = Backend::CpuReference; ///< Backend that actually ran the kernel.
    u64 items = 0;       ///< grid.count()
    u64 workgroups = 0;  ///< group_count(grid, workgroup).count()
    u64 duration_ns = 0; ///< Wall time of the launch on the calling thread (GPU: enqueue [+ sync]).
    bool ok = false;
};

/// Aggregate over every recorded launch of one kernel name.
struct KernelStats {
    const char* name = nullptr;
    u64 launches = 0;
    u64 failed_launches = 0;
    u64 items = 0;
    u64 workgroups = 0;
    u64 total_ns = 0;
    u64 last_ns = 0;
    u64 min_ns = 0;
    u64 max_ns = 0;
    Backend last_backend = Backend::CpuReference;
    u64 launches_by_backend[kBackendCount] = {};
};

/// Maximum distinct kernel names tracked; launches of further names only count in `kernel_stats_dropped()`.
inline constexpr u32 kMaxTrackedKernels = 256;

const char* backend_name(Backend backend);

/// True when `backend` can execute right now. CPU backends always; Cuda only in a CUDA build
/// (FUSE_HAS_CUDA) with a usable device; VulkanCompute never by itself (needs a launch-supplied entry).
bool backend_available(Backend backend);

void record_launch(const LaunchRecord& record);

/// Aggregated stats for `name` (compared by string content). False when never launched.
bool find_kernel_stats(const char* name, KernelStats& out);
u32 kernel_stats_count();
bool kernel_stats_at(u32 index, KernelStats& out);
/// Most recent launch recorded on any thread (default record when none).
LaunchRecord last_launch();
u64 total_launch_count();
u64 kernel_stats_dropped();
void reset_kernel_stats();

} // namespace fuse::kernel
