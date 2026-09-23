#pragma once

// kernel::launch() — runs one single-source kernel body on a backend (host API; see kernel.hpp).
//
//   CpuReference  serial, deterministic order: item kernels visit global items in linear order
//                 (x fastest, then y, then z); workgroup kernels visit workgroups in linear order.
//   CpuParallel   fuse::jobs::JobScheduler::parallel_for over workgroups (grain = whole workgroups),
//                 heap-free in steady state; each workgroup runs its local threads serially.
//   Cuda          LaunchOptions::cuda (a kernel::cuda::entry<Body, Params> from cuda_launch.cuh, compiled
//                 in a .cu TU). Params must reference device-visible memory.
//   VulkanCompute LaunchOptions::vulkan (reserved seam — no backend ships yet).
//
// Every launch opens a profiler scope named `launch.name` and records a LaunchRecord (stats.hpp).
// A GPU backend that is requested but unavailable (no CUDA build, no device, no entry) falls back to
// CpuParallel unless `allow_fallback` is false; the record carries the backend that really ran.

#include <fuse/compute_kernel/kernel.hpp>
#include <fuse/compute_kernel/stats.hpp>
#include <fuse/jobs/job_scheduler.hpp>
#include <fuse/profiler/profiler.hpp>
#include <fuse/types.hpp>

#include <chrono>

namespace fuse::kernel {

struct LaunchOptions {
    DeviceEntryFn cuda = nullptr;   ///< Device entry for Backend::Cuda / Auto.
    DeviceEntryFn vulkan = nullptr; ///< Device entry for Backend::VulkanCompute (seam).
    void* stream = nullptr;         ///< cudaStream_t / VkCommandBuffer passed to the entry.
    u32 grain_workgroups = 0;       ///< CpuParallel workgroups per job chunk; 0 = automatic.
    bool allow_fallback = true;     ///< GPU unavailable -> CpuParallel instead of failing.
    bool synchronize = true;        ///< GPU entries wait for completion before returning.
};

struct LaunchResult {
    bool ok = false;
    Backend backend = Backend::CpuReference; ///< Backend that actually ran.
    u64 items = 0;
    u64 workgroups = 0;
    u64 duration_ns = 0;
};

/// Structural validity of a launch: named, non-empty workgroup within kMaxWorkgroupSize, and a group
/// count addressable with u32 (an empty grid is valid and runs nothing).
inline bool launch_valid(const KernelLaunch& launch) {
    if (launch.name == nullptr || launch.name[0] == '\0') {
        return false;
    }
    const Dim3& wg = launch.workgroup;
    if (wg.x == 0u || wg.y == 0u || wg.z == 0u || wg.count() > kMaxWorkgroupSize) {
        return false;
    }
    if (launch.grid.count() > 0xffffffffull) {
        return false;
    }
    return group_count(launch.grid, wg).count() <= 0xffffffffull;
}

/// Backend that `launch()` will use for `requested` given `options` (Auto / fallback resolution).
/// Returns `requested` unchanged for CPU backends; Backend::Auto only when nothing can run
/// (GPU requested, unavailable, fallback disabled).
inline Backend resolve_backend(Backend requested, const LaunchOptions& options) {
    switch (requested) {
    case Backend::CpuReference:
    case Backend::CpuParallel:
        return requested;
    case Backend::Auto:
        return (options.cuda != nullptr && backend_available(Backend::Cuda)) ? Backend::Cuda : Backend::CpuParallel;
    case Backend::Cuda:
        if (options.cuda != nullptr && backend_available(Backend::Cuda)) {
            return Backend::Cuda;
        }
        break;
    case Backend::VulkanCompute:
        if (options.vulkan != nullptr) {
            return Backend::VulkanCompute;
        }
        break;
    }
    return options.allow_fallback ? Backend::CpuParallel : Backend::Auto;
}

namespace detail {

/// Decomposes a linear workgroup id (x fastest).
inline Dim3 group_coord(u32 linear, const Dim3& groups) {
    return Dim3{linear % groups.x, (linear / groups.x) % groups.y, linear / (groups.x * groups.y)};
}

/// Item kernel: the in-grid threads of one workgroup, local x fastest.
template <typename Body, typename Params>
inline void run_items_group(const Body& body, const Params& params, const KernelLaunch& launch, const Dim3& group) {
    const Dim3& wg = launch.workgroup;
    for (u32 z = 0; z < wg.z; ++z) {
        for (u32 y = 0; y < wg.y; ++y) {
            for (u32 x = 0; x < wg.x; ++x) {
                const LaunchIndex idx = make_index(launch.grid, wg, group, Dim3{x, y, z});
                if (idx.active) {
                    body(idx, params);
                }
            }
        }
    }
}

/// Workgroup kernel: every phase for every local thread (padding threads included), phase-major —
/// the CPU equivalent of `phase(); __syncthreads();` per phase.
template <typename Body, typename Params>
inline void run_workgroup(const Body& body, const Params& params, const KernelLaunch& launch, const Dim3& group) {
    using Scratch = typename Body::Scratch;
    alignas(16) unsigned char raw[sizeof(Scratch) * Body::kScratchCount];
    WorkgroupContext<Scratch> ctx{};
    ctx.scratch = reinterpret_cast<Scratch*>(raw);
    ctx.scratch_count = Body::kScratchCount;
    ctx.phase_count = Body::kPhases;
    const Dim3& wg = launch.workgroup;
    for (u32 phase = 0; phase < Body::kPhases; ++phase) {
        ctx.phase = phase;
        for (u32 z = 0; z < wg.z; ++z) {
            for (u32 y = 0; y < wg.y; ++y) {
                for (u32 x = 0; x < wg.x; ++x) {
                    body(make_index(launch.grid, wg, group, Dim3{x, y, z}), ctx, params);
                }
            }
        }
    }
}

template <typename Body, typename Params>
inline void run_cpu_reference(const Body& body, const Params& params, const KernelLaunch& launch) {
    const Dim3& grid = launch.grid;
    const Dim3& wg = launch.workgroup;
    if constexpr (is_workgroup_kernel_v<Body>) {
        const Dim3 groups = group_count(grid, wg);
        const u32 total = static_cast<u32>(groups.count());
        for (u32 g = 0; g < total; ++g) {
            run_workgroup(body, params, launch, group_coord(g, groups));
        }
    } else {
        for (u32 z = 0; z < grid.z; ++z) {
            for (u32 y = 0; y < grid.y; ++y) {
                for (u32 x = 0; x < grid.x; ++x) {
                    const Dim3 group{x / wg.x, y / wg.y, z / wg.z};
                    const Dim3 local{x % wg.x, y % wg.y, z % wg.z};
                    body(make_index(grid, wg, group, local), params);
                }
            }
        }
    }
}

template <typename Body, typename Params>
inline void run_cpu_parallel(const Body& body, const Params& params, const KernelLaunch& launch, u32 grain) {
    const Dim3 groups = group_count(launch.grid, launch.workgroup);
    const u32 total = static_cast<u32>(groups.count());
    jobs::JobScheduler& scheduler = jobs::JobScheduler::instance();
    if (grain == 0u) {
        // ~4 chunks per participating thread balances stealing against dispatch overhead.
        const u32 threads = scheduler.workerCount() + 1u;
        grain = total / (threads * 4u);
        if (grain == 0u) {
            grain = 1u;
        }
    }
    // Captures by reference only: the lambda lives on this stack frame and parallel_for reaches it by
    // pointer, so dispatch stays heap-free.
    const auto chunk = [&body, &params, &launch, groups](u32 g) {
        if constexpr (is_workgroup_kernel_v<Body>) {
            run_workgroup(body, params, launch, group_coord(g, groups));
        } else {
            run_items_group(body, params, launch, group_coord(g, groups));
        }
    };
    scheduler.parallel_for(0u, total, grain, chunk);
}

inline u64 now_ns() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace detail

/// Runs `body` over `launch.grid` on `backend` (see file comment). Thread-safe; heap-free on the CPU
/// backends in steady state.
template <typename Body, typename Params>
LaunchResult launch(Backend backend, const KernelLaunch& launch, const Body& body, const Params& params,
                    const LaunchOptions& options = {}) {
    static_assert(check_kernel_types<Body, Params>());
    LaunchResult result{};
    LaunchRecord record{};
    record.name = launch.name;
    record.requested = backend;
    if (!launch_valid(launch)) {
        record.backend = backend;
        record_launch(record);
        result.backend = backend;
        return result;
    }

    const Backend resolved = resolve_backend(backend, options);
    result.backend = resolved;
    result.items = launch.grid.count();
    result.workgroups = group_count(launch.grid, launch.workgroup).count();

    const u64 start = detail::now_ns();
    {
        FUSE_PROFILE_SCOPE(launch.name);
        switch (resolved) {
        case Backend::CpuReference:
            detail::run_cpu_reference(body, params, launch);
            result.ok = true;
            break;
        case Backend::CpuParallel:
            detail::run_cpu_parallel(body, params, launch, options.grain_workgroups);
            result.ok = true;
            break;
        case Backend::Cuda:
            result.ok = result.items == 0u || options.cuda(launch, &body, &params, options.stream, options.synchronize);
            break;
        case Backend::VulkanCompute:
            result.ok =
                result.items == 0u || options.vulkan(launch, &body, &params, options.stream, options.synchronize);
            break;
        case Backend::Auto:
            result.ok = false; // GPU requested, unavailable, fallback disabled.
            break;
        }
    }
    result.duration_ns = detail::now_ns() - start;

    record.backend = resolved;
    record.items = result.items;
    record.workgroups = result.workgroups;
    record.duration_ns = result.duration_ns;
    record.ok = result.ok;
    record_launch(record);
    return result;
}

} // namespace fuse::kernel
