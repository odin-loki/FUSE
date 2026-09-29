#pragma once

// B1.3 GPUAllocator (FUSE_MASTER_PLAN "GPU Memory Manager"): CUDA memory for compute work.
//
//   Device        cudaMalloc                       GPU only; device_ptr set, host_ptr null
//   Pinned        cudaMallocHost                   page-locked host memory for fast DMA; host_ptr set,
//                                                  device_ptr = the UVA alias when the device reports one
//   Managed       cudaMallocManaged (attach global) one pointer for host and device (host_ptr == device_ptr)
//   DeviceMapped  cudaHostAlloc(Mapped)            zero-copy host pages; host_ptr + cudaHostGetDevicePointer
//
// Vulkan memory stays in renderer::GpuAllocator (VMA); memory shared between the two APIs goes through
// renderer::cuda::import_vulkan_buffer / import_vulkan_image.
//
// Every live allocation is tracked, so free() of a pointer this allocator does not own (double free,
// foreign pointer) is refused and counted instead of reaching the CUDA runtime. Thread-safe. In builds
// without CUDA (FUSE_HAS_CUDA undefined) or without a device every allocate() fails with a reason.
// Destroying the allocator frees what is still live. Allocations are at least 256-byte aligned.

#include <fuse/types.hpp>

#include <cstddef>
#include <memory_resource>
#include <mutex>
#include <unordered_map>

namespace fuse::alloc {

enum class GPUMemoryType : u8 {
    Device = 0,
    Pinned = 1,
    Managed = 2,
    DeviceMapped = 3,
};

inline constexpr u32 kGPUMemoryTypeCount = 4;
/// Alignment every successful allocation honours (cudaMalloc / cudaMallocHost guarantee at least this).
inline constexpr usize kGPUAllocationAlignment = 256;

const char* gpu_memory_type_name(GPUMemoryType type);

struct GPUAllocation {
    void* device_ptr = nullptr; ///< Address kernels use (null for Pinned without a UVA alias).
    void* host_ptr = nullptr;   ///< Address host code uses (null for Device).
    usize size = 0;
    GPUMemoryType type = GPUMemoryType::Device;

    [[nodiscard]] bool valid() const { return device_ptr != nullptr || host_ptr != nullptr; }
};

struct GPUMemoryTypeStats {
    u64 allocations = 0; ///< Successful allocate() calls.
    u64 frees = 0;       ///< Successful free() calls (including release_all()).
    u64 failed = 0;      ///< allocate() calls that returned an invalid allocation.
    u64 live_count = 0;
    usize live_bytes = 0;
    usize peak_live_bytes = 0;
};

struct GPUAllocatorStats {
    GPUMemoryTypeStats types[kGPUMemoryTypeCount]{};
    /// free() of an allocation this allocator does not own (double free, foreign or corrupted pointer).
    u64 invalid_frees = 0;

    [[nodiscard]] const GPUMemoryTypeStats& of(GPUMemoryType type) const { return types[static_cast<u32>(type)]; }
    [[nodiscard]] u64 live_count() const;
    [[nodiscard]] usize live_bytes() const;
};

class GPUAllocator {
public:
    GPUAllocator() = default;
    ~GPUAllocator();
    GPUAllocator(const GPUAllocator&) = delete;
    GPUAllocator& operator=(const GPUAllocator&) = delete;

    /// True when this build has CUDA and the runtime reports at least one device.
    [[nodiscard]] static bool cuda_available();
    /// Why cuda_available() is false ("" when it is true).
    [[nodiscard]] static const char* unavailable_reason();

    /// Allocates `size` bytes (> 0) of `type`. Returns an invalid allocation (and counts a failure,
    /// see last_error()) when CUDA is unavailable, `size` is 0 or the runtime call fails.
    [[nodiscard]] GPUAllocation allocate(usize size, GPUMemoryType type);

    /// Frees an allocation returned by allocate() and resets it. An invalid (empty) allocation is a
    /// no-op returning true. An allocation this allocator does not own is refused (returns false,
    /// counted in GPUAllocatorStats::invalid_frees) and never reaches the CUDA runtime.
    bool free(GPUAllocation& allocation);

    /// True when `ptr` is the device or host address of a live allocation of this allocator.
    [[nodiscard]] bool owns(const void* ptr) const;

    /// Frees every live allocation; returns how many there were.
    usize release_all();

    [[nodiscard]] GPUAllocatorStats stats() const;
    /// Last failure message (static string; "" when nothing failed yet).
    [[nodiscard]] const char* last_error() const;

private:
    struct Record {
        GPUAllocation allocation{};
    };

    static const void* key_of(const GPUAllocation& allocation);
    static void release_native(const GPUAllocation& allocation);
    void note_failure(GPUMemoryType type, const char* reason);

    mutable std::mutex m_mutex;
    std::unordered_map<const void*, Record> m_live;
    GPUAllocatorStats m_stats{};
    const char* m_lastError = "";
};

/// std::pmr::memory_resource over GPUAllocator Managed memory (cudaMallocManaged), for containers whose
/// storage CUDA kernels read through the same pointer as host code (e.g. an ECS component column via
/// ecs::Registry::set_column_memory_resource). Allocation failure throws std::bad_alloc, as the pmr
/// contract requires; alignments above kGPUAllocationAlignment are refused. The GPUAllocator must
/// outlive the resource and every container using it.
class GPUManagedMemoryResource final : public std::pmr::memory_resource {
public:
    explicit GPUManagedMemoryResource(GPUAllocator& allocator) : m_allocator(&allocator) {}

    [[nodiscard]] GPUAllocator& allocator() const { return *m_allocator; }
    [[nodiscard]] u64 allocation_count() const;
    [[nodiscard]] u64 deallocation_count() const;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override;
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override;
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    GPUAllocator* m_allocator = nullptr;
    mutable std::mutex m_mutex;
    u64 m_allocations = 0;
    u64 m_deallocations = 0;
};

} // namespace fuse::alloc
