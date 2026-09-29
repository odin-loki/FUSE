#include <fuse/alloc/gpu_allocator.hpp>

#include <algorithm>
#include <new>
#include <utility>
#include <vector>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

namespace fuse::alloc {

const char* gpu_memory_type_name(GPUMemoryType type) {
    switch (type) {
    case GPUMemoryType::Device:
        return "Device";
    case GPUMemoryType::Pinned:
        return "Pinned";
    case GPUMemoryType::Managed:
        return "Managed";
    case GPUMemoryType::DeviceMapped:
        return "DeviceMapped";
    }
    return "Unknown";
}

u64 GPUAllocatorStats::live_count() const {
    u64 total = 0;
    for (const GPUMemoryTypeStats& t : types) {
        total += t.live_count;
    }
    return total;
}

usize GPUAllocatorStats::live_bytes() const {
    usize total = 0;
    for (const GPUMemoryTypeStats& t : types) {
        total += t.live_bytes;
    }
    return total;
}

GPUAllocator::~GPUAllocator() {
    (void)release_all();
}

bool GPUAllocator::cuda_available() {
#if defined(FUSE_HAS_CUDA)
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
#else
    return false;
#endif
}

const char* GPUAllocator::unavailable_reason() {
#if defined(FUSE_HAS_CUDA)
    int count = 0;
    const cudaError_t err = cudaGetDeviceCount(&count);
    if (err != cudaSuccess) {
        return cudaGetErrorString(err);
    }
    return count > 0 ? "" : "no CUDA device";
#else
    return "CUDA not compiled in (FUSE_BUILD_CUDA=OFF or toolkit not found)";
#endif
}

const void* GPUAllocator::key_of(const GPUAllocation& allocation) {
    return allocation.type == GPUMemoryType::Device ? allocation.device_ptr : allocation.host_ptr;
}

void GPUAllocator::note_failure(GPUMemoryType type, const char* reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_stats.types[static_cast<u32>(type)].failed;
    m_lastError = reason;
}

GPUAllocation GPUAllocator::allocate(usize size, GPUMemoryType type) {
    if (static_cast<u32>(type) >= kGPUMemoryTypeCount) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastError = "GPUAllocator::allocate: unknown GPUMemoryType";
        return {};
    }
    if (size == 0u) {
        note_failure(type, "GPUAllocator::allocate: zero-byte request");
        return {};
    }

    GPUAllocation allocation{};
    allocation.size = size;
    allocation.type = type;

#if defined(FUSE_HAS_CUDA)
    cudaError_t err = cudaSuccess;
    switch (type) {
    case GPUMemoryType::Device:
        err = cudaMalloc(&allocation.device_ptr, size);
        break;
    case GPUMemoryType::Pinned:
        err = cudaMallocHost(&allocation.host_ptr, size);
        if (err == cudaSuccess) {
            // Under unified addressing page-locked memory is device-accessible through the same pointer;
            // report the alias when the runtime confirms it (never fail the allocation over it).
            void* alias = nullptr;
            if (cudaHostGetDevicePointer(&alias, allocation.host_ptr, 0) == cudaSuccess) {
                allocation.device_ptr = alias;
            } else {
                (void)cudaGetLastError();
            }
        }
        break;
    case GPUMemoryType::Managed:
        err = cudaMallocManaged(&allocation.device_ptr, size, cudaMemAttachGlobal);
        allocation.host_ptr = allocation.device_ptr;
        break;
    case GPUMemoryType::DeviceMapped:
        err = cudaHostAlloc(&allocation.host_ptr, size, cudaHostAllocMapped);
        if (err == cudaSuccess) {
            err = cudaHostGetDevicePointer(&allocation.device_ptr, allocation.host_ptr, 0);
            if (err != cudaSuccess) {
                (void)cudaFreeHost(allocation.host_ptr);
            }
        }
        break;
    }
    if (err != cudaSuccess) {
        (void)cudaGetLastError(); // clear the (non-sticky) allocation error
        note_failure(type, cudaGetErrorString(err));
        return {};
    }
#else
    note_failure(type, unavailable_reason());
    return {};
#endif

    std::lock_guard<std::mutex> lock(m_mutex);
    m_live[key_of(allocation)] = Record{allocation};
    GPUMemoryTypeStats& t = m_stats.types[static_cast<u32>(type)];
    ++t.allocations;
    ++t.live_count;
    t.live_bytes += size;
    t.peak_live_bytes = std::max(t.peak_live_bytes, t.live_bytes);
    return allocation;
}

void GPUAllocator::release_native(const GPUAllocation& allocation) {
#if defined(FUSE_HAS_CUDA)
    switch (allocation.type) {
    case GPUMemoryType::Device:
    case GPUMemoryType::Managed:
        (void)cudaFree(allocation.device_ptr);
        break;
    case GPUMemoryType::Pinned:
    case GPUMemoryType::DeviceMapped:
        (void)cudaFreeHost(allocation.host_ptr);
        break;
    }
#else
    (void)allocation;
#endif
}

bool GPUAllocator::free(GPUAllocation& allocation) {
    if (!allocation.valid()) {
        return true;
    }
    Record record{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_live.find(key_of(allocation));
        const bool matches = it != m_live.end() && it->second.allocation.type == allocation.type &&
                             it->second.allocation.size == allocation.size &&
                             it->second.allocation.device_ptr == allocation.device_ptr &&
                             it->second.allocation.host_ptr == allocation.host_ptr;
        if (!matches) {
            ++m_stats.invalid_frees;
            m_lastError = "GPUAllocator::free: allocation not owned (double free or foreign pointer)";
            return false;
        }
        record = it->second;
        m_live.erase(it);
        GPUMemoryTypeStats& t = m_stats.types[static_cast<u32>(record.allocation.type)];
        ++t.frees;
        --t.live_count;
        t.live_bytes -= record.allocation.size;
    }
    release_native(record.allocation);
    allocation = GPUAllocation{};
    return true;
}

bool GPUAllocator::owns(const void* ptr) const {
    if (ptr == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& entry : m_live) {
        const GPUAllocation& a = entry.second.allocation;
        if (a.device_ptr == ptr || a.host_ptr == ptr) {
            return true;
        }
    }
    return false;
}

usize GPUAllocator::release_all() {
    std::vector<GPUAllocation> released;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        released.reserve(m_live.size());
        for (const auto& entry : m_live) {
            const GPUAllocation& a = entry.second.allocation;
            GPUMemoryTypeStats& t = m_stats.types[static_cast<u32>(a.type)];
            ++t.frees;
            --t.live_count;
            t.live_bytes -= a.size;
            released.push_back(a);
        }
        m_live.clear();
    }
    for (const GPUAllocation& a : released) {
        release_native(a);
    }
    return released.size();
}

GPUAllocatorStats GPUAllocator::stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stats;
}

const char* GPUAllocator::last_error() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

u64 GPUManagedMemoryResource::allocation_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_allocations;
}

u64 GPUManagedMemoryResource::deallocation_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_deallocations;
}

void* GPUManagedMemoryResource::do_allocate(std::size_t bytes, std::size_t alignment) {
    if (alignment > kGPUAllocationAlignment) {
        throw std::bad_alloc{};
    }
    const GPUAllocation allocation = m_allocator->allocate(bytes == 0u ? 1u : bytes, GPUMemoryType::Managed);
    if (!allocation.valid()) {
        throw std::bad_alloc{};
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_allocations;
    return allocation.host_ptr;
}

void GPUManagedMemoryResource::do_deallocate(void* p, std::size_t bytes, std::size_t /*alignment*/) {
    if (p == nullptr) {
        return;
    }
    GPUAllocation allocation{};
    allocation.device_ptr = p;
    allocation.host_ptr = p;
    allocation.size = bytes == 0u ? 1u : bytes;
    allocation.type = GPUMemoryType::Managed;
    if (m_allocator->free(allocation)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_deallocations;
    }
}

bool GPUManagedMemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    const auto* managed = dynamic_cast<const GPUManagedMemoryResource*>(&other);
    return managed != nullptr && managed->m_allocator == m_allocator;
}

} // namespace fuse::alloc
