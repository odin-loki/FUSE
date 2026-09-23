#pragma once

// Single-source compute kernel model (see docs/compute-kernels.md).
//
// A kernel body is a trivially copyable, stateless-or-POD functor whose call operator is
// FUSE_HOST_DEVICE. The same body runs on every backend: serially on the CPU (CpuReference),
// fanned out over fuse::jobs (CpuParallel), and as a CUDA __global__ trampoline (Cuda). This header
// is device-safe: it is included by .cu translation units (nvcc, C++20) as well as C++23 host code.
//
// Two body forms:
//
//   Item kernel — one call per grid item, no cross-item communication:
//     struct MyKernel {
//         FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx, const MyParams& p) const;
//     };
//
//   Workgroup kernel — shared scratch + bulk-synchronous phases instead of barriers:
//     struct MyTiledKernel {
//         using Scratch = u32;                        // element type of the workgroup scratch
//         static constexpr u32 kScratchCount = 256;   // elements (<= kMaxWorkgroupScratchBytes total)
//         static constexpr u32 kPhases = 3;           // phase p+1 sees every write of phase p
//         FUSE_HOST_DEVICE void operator()(const kernel::LaunchIndex& idx,
//                                          const kernel::WorkgroupContext<u32>& wg,
//                                          const MyParams& p) const;
//     };
//   Every local thread of a workgroup runs phase 0, then every thread runs phase 1, ... On CUDA the
//   scratch is __shared__ memory and phases are separated by __syncthreads(). State that must survive
//   from one phase to the next lives in scratch (locals do not persist between phase calls on the CPU).
//   Padding threads of a partial edge workgroup are invoked with `idx.active == false` (they must take
//   part in scratch protocols but must not touch grid-indexed memory).

#include <fuse/types.hpp>

#include <type_traits>

namespace fuse::kernel {

/// 3D extent / coordinate. Unused dimensions stay 1 (extent) or 0 (coordinate).
struct Dim3 {
    u32 x = 1;
    u32 y = 1;
    u32 z = 1;

    FUSE_HOST_DEVICE constexpr u64 count() const { return static_cast<u64>(x) * y * z; }
};

FUSE_HOST_DEVICE constexpr Dim3 extent1(u32 x) { return Dim3{x, 1u, 1u}; }
FUSE_HOST_DEVICE constexpr Dim3 extent2(u32 x, u32 y) { return Dim3{x, y, 1u}; }
FUSE_HOST_DEVICE constexpr Dim3 extent3(u32 x, u32 y, u32 z) { return Dim3{x, y, z}; }

/// ceil(a / b) without overflowing for `a` near 2^32 (b > 0).
FUSE_HOST_DEVICE constexpr u32 div_up(u32 a, u32 b) { return a / b + (a % b != 0u ? 1u : 0u); }

/// Number of workgroups per dimension needed to cover `grid` with `workgroup`-sized groups.
FUSE_HOST_DEVICE constexpr Dim3 group_count(const Dim3& grid, const Dim3& workgroup) {
    return Dim3{div_up(grid.x, workgroup.x), div_up(grid.y, workgroup.y), div_up(grid.z, workgroup.z)};
}

/// Where one kernel invocation sits in the launch. `global = group * workgroup + local`.
struct LaunchIndex {
    Dim3 global{0u, 0u, 0u};
    Dim3 local{0u, 0u, 0u};
    Dim3 group{0u, 0u, 0u};
    Dim3 grid{};      ///< Launch grid extent (items).
    Dim3 workgroup{}; ///< Workgroup extent (threads).
    u32 linear = 0;       ///< global.x + global.y * grid.x + global.z * grid.x * grid.y
    u32 local_linear = 0; ///< local.x + local.y * workgroup.x + local.z * workgroup.x * workgroup.y
    bool active = true;   ///< False only for workgroup-kernel padding threads outside the grid.
};

/// Builds the index of local thread `local` of workgroup `group` (identical on every backend).
FUSE_HOST_DEVICE constexpr LaunchIndex make_index(const Dim3& grid, const Dim3& workgroup, const Dim3& group,
                                                  const Dim3& local) {
    LaunchIndex idx{};
    idx.grid = grid;
    idx.workgroup = workgroup;
    idx.group = group;
    idx.local = local;
    idx.global = Dim3{group.x * workgroup.x + local.x, group.y * workgroup.y + local.y,
                      group.z * workgroup.z + local.z};
    idx.active = idx.global.x < grid.x && idx.global.y < grid.y && idx.global.z < grid.z;
    idx.linear = idx.global.x + idx.global.y * grid.x + idx.global.z * grid.x * grid.y;
    idx.local_linear = local.x + local.y * workgroup.x + local.z * workgroup.x * workgroup.y;
    return idx;
}

/// Execution backend. `Auto` resolves to the fastest available one (Cuda when a device and a device
/// entry exist, otherwise CpuParallel). `VulkanCompute` is the reserved seam for a SPIR-V backend:
/// it is used only when the launch supplies a Vulkan entry (LaunchOptions::vulkan), else it falls back.
enum class Backend : u8 { Auto = 0, CpuReference = 1, CpuParallel = 2, Cuda = 3, VulkanCompute = 4 };

inline constexpr u32 kBackendCount = 5;

/// Portable limits: CUDA caps a block at 1024 threads; Vulkan only guarantees 128 invocations per
/// workgroup (keep GPU-bound kernels at <= 256). Scratch is capped so the CPU emulation can keep it
/// on the (64 KiB) fiber stack and CUDA can use static __shared__ memory.
inline constexpr u32 kMaxWorkgroupSize = 1024;
inline constexpr u32 kMaxWorkgroupScratchBytes = 16384;

/// A named dispatch. `name` must have static storage duration (string literal): it is the profiler
/// scope name, the stats-registry key, and the name a GPU timestamp query uses for the same pass.
struct KernelLaunch {
    const char* name = nullptr;
    Dim3 grid{};                ///< Total items per dimension.
    Dim3 workgroup{64u, 1u, 1u}; ///< Threads per workgroup (CUDA block / Vulkan local_size).
};

/// Non-owning device-safe view (a POD pointer + count usable inside kernel params on every backend).
template <typename T>
struct Span {
    T* data = nullptr;
    u32 size = 0;

    FUSE_HOST_DEVICE constexpr T& operator[](u32 i) const { return data[i]; }
    FUSE_HOST_DEVICE constexpr bool empty() const { return size == 0u; }
    FUSE_HOST_DEVICE constexpr T* begin() const { return data; }
    FUSE_HOST_DEVICE constexpr T* end() const { return data + size; }
};

template <typename T>
FUSE_HOST_DEVICE constexpr Span<T> make_span(T* data, u32 size) {
    return Span<T>{data, size};
}

/// Per-workgroup state handed to a workgroup kernel: its scratch and the current phase.
template <typename T>
struct WorkgroupContext {
    T* scratch = nullptr;
    u32 scratch_count = 0;
    u32 phase = 0;
    u32 phase_count = 1;
};

/// True for bodies declaring the workgroup form (Scratch / kScratchCount / kPhases).
template <typename Body, typename = void>
struct is_workgroup_kernel : std::false_type {};

template <typename Body>
struct is_workgroup_kernel<Body, std::void_t<typename Body::Scratch, decltype(Body::kScratchCount),
                                             decltype(Body::kPhases)>> : std::true_type {};

template <typename Body>
inline constexpr bool is_workgroup_kernel_v = is_workgroup_kernel<Body>::value;

/// Compile-time checks shared by every backend (host and nvcc).
template <typename Body, typename Params>
constexpr bool check_kernel_types() {
    static_assert(std::is_trivially_copyable_v<Body>, "kernel bodies are copied to the device: trivially copyable");
    static_assert(std::is_trivially_copyable_v<Params>,
                  "kernel params are copied by value to the device: POD pointers/Spans/scalars only");
    if constexpr (is_workgroup_kernel_v<Body>) {
        static_assert(Body::kPhases >= 1u, "workgroup kernels need at least one phase");
        static_assert(Body::kScratchCount >= 1u, "workgroup kernels need at least one scratch element");
        static_assert(sizeof(typename Body::Scratch) * Body::kScratchCount <= kMaxWorkgroupScratchBytes,
                      "workgroup scratch exceeds kMaxWorkgroupScratchBytes");
        static_assert(std::is_trivially_copyable_v<typename Body::Scratch>, "scratch elements must be POD");
        static_assert(alignof(typename Body::Scratch) <= 16u, "scratch alignment is 16 bytes");
    }
    return true;
}

/// Type-erased device entry: `body` / `params` point at the typed Body / Params the entry was
/// instantiated for; `stream` is a cudaStream_t (Cuda) or VkCommandBuffer (VulkanCompute);
/// `synchronize` asks the entry to wait for completion before returning. Returns false on failure.
using DeviceEntryFn = bool (*)(const KernelLaunch& launch, const void* body, const void* params, void* stream,
                               bool synchronize);

} // namespace fuse::kernel
