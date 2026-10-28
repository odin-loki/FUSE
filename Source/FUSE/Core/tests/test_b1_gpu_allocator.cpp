// B1.3 GPUAllocator contract gate (runs in every build, with or without CUDA / a device):
//   - allocate(0) fails for every type; free of an empty allocation is a no-op; free of a pointer the
//     allocator does not own is refused (counted in invalid_frees) and never reaches the CUDA runtime.
//   - Without CUDA (build or device): every type fails with a reason, nothing is live, and a pmr
//     container on GPUManagedMemoryResource throws std::bad_alloc.
//   - With a device: Device / Pinned / Managed / DeviceMapped allocate and free, 256-byte aligned,
//     host-visible types are writable through host_ptr, stats balance (live 0 at the end), a double free
//     is refused, and a pmr vector grows inside Managed memory.
// The kernel-side checks (device writes/reads of every type, 1k-cycle stress, cudaMemGetInfo leak check)
// live in fuse_gpu_allocator_device_gate; fuse_gpu_allocator_memcheck runs that under compute-sanitizer.

#include <fuse/alloc/gpu_allocator.hpp>
#include <fuse/core/gpu_block_count.hpp>
#include <fuse/gameplay/magazine.hpp>
#include <fuse/memory/gpu_block.hpp>

#include "fuse_test_msvc_debug_stl.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory_resource>
#include <new>
#include <vector>

namespace {

using fuse::u32;
using fuse::u64;
using fuse::usize;
using fuse::alloc::GPUAllocation;
using fuse::alloc::GPUAllocator;
using fuse::alloc::GPUMemoryType;

int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

constexpr GPUMemoryType kTypes[] = {GPUMemoryType::Device, GPUMemoryType::Pinned, GPUMemoryType::Managed,
                                    GPUMemoryType::DeviceMapped};

bool aligned256(const void* p) {
    return p == nullptr || (reinterpret_cast<std::uintptr_t>(p) % fuse::alloc::kGPUAllocationAlignment) == 0u;
}

void testGpuBlockCountOk() {
    expectTrue(fuse::core::gpu_block_count_ok(1), "GPU block count 1 is valid");
    expectTrue(fuse::core::gpu_block_count_ok(4096), "GPU block count 4096 is valid");
    expectTrue(!fuse::core::gpu_block_count_ok(0), "GPU block count 0 is below the minimum");
    expectTrue(!fuse::core::gpu_block_count_ok(4097), "GPU block count 4097 is above the maximum");
}

void testGpuBlockOk() {
    expectTrue(fuse::memory::gpu_block_ok(256u), "256-byte GPU block is valid");
    expectTrue(fuse::memory::gpu_block_ok(4096u), "4096-byte GPU block is valid");
    expectTrue(!fuse::memory::gpu_block_ok(128u), "128-byte GPU block is below the minimum");
    expectTrue(!fuse::memory::gpu_block_ok(300u), "300-byte GPU block is not a power of two");
}

void testContract() {
    GPUAllocator allocator;
    for (GPUMemoryType type : kTypes) {
        GPUAllocation zero = allocator.allocate(0, type);
        expectTrue(!zero.valid(), "allocate(0) returns an invalid allocation");
        expectTrue(allocator.stats().of(type).failed == 1u, "allocate(0) counted as a failure");
    }
    GPUAllocation empty{};
    expectTrue(allocator.free(empty), "free of an empty allocation is a no-op");

    alignas(256) static unsigned char foreign[512];
    GPUAllocation bogus{};
    bogus.device_ptr = foreign;
    bogus.host_ptr = foreign;
    bogus.size = sizeof(foreign);
    bogus.type = GPUMemoryType::Managed;
    expectTrue(!allocator.free(bogus), "free of a foreign pointer is refused");
    expectTrue(allocator.stats().invalid_frees == 1u, "foreign free counted in invalid_frees");
    expectTrue(bogus.valid(), "refused free leaves the caller's allocation untouched");
    expectTrue(!allocator.owns(foreign), "owns() is false for foreign memory");
    expectTrue(std::strlen(allocator.last_error()) > 0u, "last_error() explains the refusal");
}

void testWithoutCuda() {
    if (fuse_test_msvc_debug_stl()) {
        std::printf("GPUAllocator: MSVC debug STL — no-device contract probe skipped\n");
        return;
    }
    GPUAllocator allocator;
    std::printf("GPUAllocator: CUDA unavailable (%s) - checking the no-device contract\n",
                GPUAllocator::unavailable_reason());
    expectTrue(std::strlen(GPUAllocator::unavailable_reason()) > 0u, "unavailable_reason() is set");
    for (GPUMemoryType type : kTypes) {
        GPUAllocation a = allocator.allocate(4096, type);
        expectTrue(!a.valid() && a.device_ptr == nullptr && a.host_ptr == nullptr,
                   "allocation fails without CUDA");
        expectTrue(allocator.stats().of(type).failed == 1u && allocator.stats().of(type).allocations == 0u,
                   "failed allocation counted, nothing allocated");
    }
    expectTrue(allocator.stats().live_count() == 0u && allocator.stats().live_bytes() == 0u, "nothing is live");

    fuse::alloc::GPUManagedMemoryResource resource(allocator);
    bool threw = false;
    try {
        std::pmr::vector<float> column(&resource);
        column.resize(1024);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    expectTrue(threw, "pmr container on GPUManagedMemoryResource throws std::bad_alloc without CUDA");
    expectTrue(resource.allocation_count() == 0u, "no managed allocation recorded");
}

void testWithCuda() {
    GPUAllocator allocator;
    const usize sizes[] = {1u, 255u, 256u, 4097u, 1u << 20};
    std::vector<GPUAllocation> live;
    for (GPUMemoryType type : kTypes) {
        for (usize size : sizes) {
            GPUAllocation a = allocator.allocate(size, type);
            char label[128];
            std::snprintf(label, sizeof(label), "%s allocation of %llu bytes", fuse::alloc::gpu_memory_type_name(type),
                          static_cast<unsigned long long>(size));
            expectTrue(a.valid() && a.size == size && a.type == type, label);
            if (!a.valid()) {
                std::fprintf(stderr, "  reason: %s\n", allocator.last_error());
                continue;
            }
            expectTrue(aligned256(a.device_ptr) && aligned256(a.host_ptr), "allocation is 256-byte aligned");
            expectTrue(type == GPUMemoryType::Device ? (a.host_ptr == nullptr && a.device_ptr != nullptr)
                                                     : a.host_ptr != nullptr,
                       "Device has no host pointer; host-visible types have one");
            if (type == GPUMemoryType::Managed) {
                expectTrue(a.host_ptr == a.device_ptr, "Managed host_ptr == device_ptr");
            }
            if (type == GPUMemoryType::DeviceMapped) {
                expectTrue(a.device_ptr != nullptr, "DeviceMapped has a device pointer");
            }
            if (a.host_ptr != nullptr) {
                std::memset(a.host_ptr, 0x5a, size);
                const auto* bytes = static_cast<const unsigned char*>(a.host_ptr);
                expectTrue(bytes[0] == 0x5a && bytes[size - 1u] == 0x5a, "host-visible memory is host-writable");
            }
            expectTrue(allocator.owns(a.device_ptr != nullptr ? a.device_ptr : a.host_ptr), "owns() live allocation");
            live.push_back(a);
        }
    }
    const fuse::alloc::GPUAllocatorStats mid = allocator.stats();
    expectTrue(mid.live_count() == live.size(), "stats count every live allocation");
    for (GPUAllocation& a : live) {
        GPUAllocation copy = a;
        expectTrue(allocator.free(a), "free succeeds");
        expectTrue(!a.valid(), "free resets the allocation");
        expectTrue(!allocator.free(copy), "double free is refused");
    }
    const fuse::alloc::GPUAllocatorStats end = allocator.stats();
    expectTrue(end.live_count() == 0u && end.live_bytes() == 0u, "nothing live after freeing everything");
    expectTrue(end.invalid_frees == live.size(), "every double free counted");
    for (GPUMemoryType type : kTypes) {
        const fuse::alloc::GPUMemoryTypeStats& t = end.of(type);
        expectTrue(t.allocations == t.frees && t.peak_live_bytes > 0u, "allocations == frees per type");
    }

    fuse::alloc::GPUManagedMemoryResource resource(allocator);
    {
        std::pmr::vector<u32> column(&resource);
        for (u32 i = 0; i < 100000u; ++i) {
            column.push_back(i * 3u);
        }
        expectTrue(allocator.owns(column.data()), "pmr vector storage lives in Managed memory");
        bool ok = true;
        for (u32 i = 0; i < 100000u; ++i) {
            ok = ok && column[i] == i * 3u;
        }
        expectTrue(ok, "pmr vector contents survive growth in Managed memory");
    }
    expectTrue(resource.allocation_count() > 0u && resource.allocation_count() == resource.deallocation_count(),
               "every managed pmr allocation was returned");
    expectTrue(allocator.stats().live_count() == 0u, "nothing live after the pmr container died");

    GPUAllocation leaked = allocator.allocate(1u << 16, GPUMemoryType::Device);
    expectTrue(leaked.valid(), "allocation for release_all");
    expectTrue(allocator.release_all() == 1u && allocator.stats().live_count() == 0u, "release_all frees what is live");
}

void testMagazineReload() {
    fuse::gameplay::Magazine mag;
    mag.rounds = 2u;
    mag.capacity = 2u;
    mag.reloadRemaining = 0.f;
    mag.reloadDuration = 1.5f;

    expectTrue(fuse::gameplay::try_fire(mag), "first shot fires");
    expectTrue(mag.rounds == 1u, "first shot consumes one round");
    expectTrue(fuse::gameplay::try_fire(mag), "second shot fires");
    expectTrue(mag.rounds == 0u, "second shot empties a capacity-2 magazine");
    expectTrue(!fuse::gameplay::try_fire(mag), "third shot fails");
    expectTrue(mag.rounds == 0u, "failed shot leaves the magazine empty");
    expectTrue(fuse::gameplay::start_reload(mag), "empty magazine starts reloading");
    expectTrue(mag.reloadRemaining == mag.reloadDuration, "reload timer starts at the duration");
    fuse::gameplay::tick_reload(mag, mag.reloadDuration);
    expectTrue(mag.rounds == 2u, "ticking the reload duration refills to capacity");
    expectTrue(mag.reloadRemaining == 0.f, "completed reload clears the timer");
}

} // namespace

int main() {
    testGpuBlockCountOk();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_b1_gpu_allocator: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    testGpuBlockOk();
    testContract();
    if (GPUAllocator::cuda_available()) {
        testWithCuda();
    } else {
        testWithoutCuda();
    }
    testMagazineReload();
    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_core_b1_gpu_allocator: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_core_b1_gpu_allocator: all checks passed (%s)\n",
                GPUAllocator::cuda_available() ? "CUDA device" : "no-device contract");
    return EXIT_SUCCESS;
}
