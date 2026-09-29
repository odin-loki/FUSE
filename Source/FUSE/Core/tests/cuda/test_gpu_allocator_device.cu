// B1 row "GPUAllocator allocates and frees Device, Pinned, Managed memory — verified with cuda-memcheck".
// Device gate (RTX 3090; exit 77 without a CUDA device). Run it plain (fuse_gpu_allocator_device_gate)
// and under compute-sanitizer memcheck --leak-check full (fuse_gpu_allocator_memcheck), which must report
// 0 errors and 0 leaks.
//   1. Every type (Device, Pinned, Managed, DeviceMapped) at sizes 1 B .. 64 MiB+3: a kernel writes a
//      byte pattern through device_ptr (Pinned through its UVA alias), the host checks it (cudaMemcpy for
//      Device, host_ptr otherwise); the host writes a second pattern and a kernel verifies it.
//   2. 1000 alloc/free cycles over random types/sizes with up to 32 live allocations; stats balance.
//   3. Double free is refused without a CUDA error; cudaMemGetInfo free bytes return to the start value.
//   --probe: exit 0 when a device exists, 77 otherwise (used by the compute-sanitizer wrapper).

#include <fuse/alloc/gpu_allocator.hpp>

#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

__host__ __device__ inline unsigned char patternByte(usize i, u32 seed) {
    u32 x = static_cast<u32>(i) * 0x9E3779B1u ^ seed * 0x85EBCA77u;
    x ^= x >> 15;
    return static_cast<unsigned char>(x & 0xffu);
}

__global__ void writePattern(unsigned char* data, usize n, u32 seed) {
    const usize stride = static_cast<usize>(blockDim.x) * gridDim.x;
    for (usize i = static_cast<usize>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
        data[i] = patternByte(i, seed);
    }
}

__global__ void countMismatches(const unsigned char* data, usize n, u32 seed, unsigned long long* mismatches) {
    const usize stride = static_cast<usize>(blockDim.x) * gridDim.x;
    unsigned long long local = 0;
    for (usize i = static_cast<usize>(blockIdx.x) * blockDim.x + threadIdx.x; i < n; i += stride) {
        local += data[i] != patternByte(i, seed) ? 1u : 0u;
    }
    if (local != 0u) {
        atomicAdd(mismatches, local);
    }
}

unsigned int gridFor(usize n) {
    const usize blocks = (n + 255u) / 256u;
    return static_cast<unsigned int>(blocks > 1024u ? 1024u : (blocks == 0u ? 1u : blocks));
}

bool cudaOk(cudaError_t err, const char* what) {
    if (err != cudaSuccess) {
        std::fprintf(stderr, "FAIL: %s: %s\n", what, cudaGetErrorString(err));
        ++g_failures;
        return false;
    }
    return true;
}

u64 hostMismatches(const unsigned char* bytes, usize n, u32 seed) {
    u64 bad = 0;
    for (usize i = 0; i < n; ++i) {
        bad += bytes[i] != patternByte(i, seed) ? 1u : 0u;
    }
    return bad;
}

void roundTrip(GPUAllocator& allocator, GPUMemoryType type, usize size, unsigned long long* deviceCounter) {
    const char* name = fuse::alloc::gpu_memory_type_name(type);
    GPUAllocation a = allocator.allocate(size, type);
    char label[160];
    std::snprintf(label, sizeof(label), "%s allocate %llu bytes (%s)", name, static_cast<unsigned long long>(size),
                  a.valid() ? "ok" : allocator.last_error());
    expectTrue(a.valid(), label);
    if (!a.valid()) {
        return;
    }
    expectTrue(reinterpret_cast<std::uintptr_t>(a.device_ptr != nullptr ? a.device_ptr : a.host_ptr) % 256u == 0u,
               "allocation 256-byte aligned");

    // 1) device writes, host reads.
    const u32 seedA = static_cast<u32>(size) * 7u + static_cast<u32>(type);
    if (a.device_ptr != nullptr) {
        writePattern<<<gridFor(size), 256>>>(static_cast<unsigned char*>(a.device_ptr), size, seedA);
        (void)cudaOk(cudaGetLastError(), "writePattern launch");
        (void)cudaOk(cudaDeviceSynchronize(), "writePattern sync");
        u64 bad = 0;
        if (type == GPUMemoryType::Device) {
            std::vector<unsigned char> host(size);
            (void)cudaOk(cudaMemcpy(host.data(), a.device_ptr, size, cudaMemcpyDeviceToHost), "D2H copy");
            bad = hostMismatches(host.data(), size, seedA);
        } else {
            bad = hostMismatches(static_cast<const unsigned char*>(a.host_ptr), size, seedA);
        }
        std::snprintf(label, sizeof(label), "%s %llu B: device-written pattern read back on the host (%llu bad)", name,
                      static_cast<unsigned long long>(size), static_cast<unsigned long long>(bad));
        expectTrue(bad == 0u, label);
    } else {
        std::printf("note: %s has no device alias on this system (host-only checks)\n", name);
    }

    // 2) host writes, device reads.
    const u32 seedB = seedA ^ 0xA5A5u;
    std::vector<unsigned char> source(size);
    for (usize i = 0; i < size; ++i) {
        source[i] = patternByte(i, seedB);
    }
    if (type == GPUMemoryType::Device) {
        (void)cudaOk(cudaMemcpy(a.device_ptr, source.data(), size, cudaMemcpyHostToDevice), "H2D copy");
    } else {
        std::memcpy(a.host_ptr, source.data(), size);
    }
    if (a.device_ptr != nullptr) {
        *deviceCounter = 0u;
        countMismatches<<<gridFor(size), 256>>>(static_cast<const unsigned char*>(a.device_ptr), size, seedB,
                                                deviceCounter);
        (void)cudaOk(cudaGetLastError(), "countMismatches launch");
        (void)cudaOk(cudaDeviceSynchronize(), "countMismatches sync");
        std::snprintf(label, sizeof(label), "%s %llu B: host-written pattern verified by a kernel (%llu bad)", name,
                      static_cast<unsigned long long>(size), *deviceCounter);
        expectTrue(*deviceCounter == 0u, label);
    }
    expectTrue(allocator.free(a) && !a.valid(), "free succeeds and resets the allocation");
}

/// Deterministic LCG.
struct Rng {
    u32 state = 0xC0DA5A5Au;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

} // namespace

int main(int argc, char** argv) {
    const bool probe = argc > 1 && std::strcmp(argv[1], "--probe") == 0;
    if (!GPUAllocator::cuda_available()) {
        std::printf("SKIP fuse_gpu_allocator_device_gate: %s\n", GPUAllocator::unavailable_reason());
        return 77;
    }
    if (probe) {
        std::printf("fuse_gpu_allocator_device_gate: CUDA device present\n");
        return 0;
    }
    cudaDeviceProp prop{};
    (void)cudaGetDeviceProperties(&prop, 0);
    int managed = 0;
    int unified = 0;
    int concurrentManaged = 0;
    (void)cudaDeviceGetAttribute(&managed, cudaDevAttrManagedMemory, 0);
    (void)cudaDeviceGetAttribute(&unified, cudaDevAttrUnifiedAddressing, 0);
    (void)cudaDeviceGetAttribute(&concurrentManaged, cudaDevAttrConcurrentManagedAccess, 0);
    std::printf("fuse_gpu_allocator_device_gate: device 0 '%s' (sm_%d%d), managed=%d, UVA=%d, concurrent managed=%d\n",
                prop.name, prop.major, prop.minor, managed, unified, concurrentManaged);
    (void)cudaFree(nullptr); // create the context before measuring free memory

    size_t freeStart = 0;
    size_t total = 0;
    (void)cudaOk(cudaMemGetInfo(&freeStart, &total), "cudaMemGetInfo (start)");
    {
        GPUAllocator allocator;
        GPUAllocation counter = allocator.allocate(sizeof(unsigned long long), GPUMemoryType::Managed);
        expectTrue(counter.valid(), "managed mismatch counter");
        if (!counter.valid()) {
            std::fprintf(stderr, "fuse_gpu_allocator_device_gate: cannot allocate managed memory (%s)\n",
                         allocator.last_error());
            return EXIT_FAILURE;
        }
        auto* deviceCounter = static_cast<unsigned long long*>(counter.host_ptr);

        const usize sizes[] = {1u, 3u, 256u, 4099u, 1u << 20, (64u << 20) + 3u};
        for (GPUMemoryType type : {GPUMemoryType::Device, GPUMemoryType::Pinned, GPUMemoryType::Managed,
                                   GPUMemoryType::DeviceMapped}) {
            for (usize size : sizes) {
                roundTrip(allocator, type, size, deviceCounter);
            }
        }

        // Stress: random types and sizes, up to 32 live, 1000 cycles, touch every allocation once.
        Rng rng{};
        std::vector<GPUAllocation> live;
        u64 cycles = 0;
        for (u32 i = 0; i < 1000u; ++i) {
            const auto type = static_cast<GPUMemoryType>(rng.next() % fuse::alloc::kGPUMemoryTypeCount);
            const usize size = 1u + rng.next() % (1u << 18);
            GPUAllocation a = allocator.allocate(size, type);
            expectTrue(a.valid(), "stress allocation");
            if (a.valid() && a.device_ptr != nullptr) {
                writePattern<<<gridFor(size), 256>>>(static_cast<unsigned char*>(a.device_ptr), size, i);
            }
            live.push_back(a);
            if (live.size() > 32u || (rng.next() & 1u) != 0u) {
                const usize victim = rng.next() % live.size();
                (void)cudaOk(cudaDeviceSynchronize(), "stress sync");
                expectTrue(allocator.free(live[victim]), "stress free");
                live[victim] = live.back();
                live.pop_back();
                ++cycles;
            }
        }
        (void)cudaOk(cudaDeviceSynchronize(), "stress final sync");
        for (GPUAllocation& a : live) {
            expectTrue(allocator.free(a), "stress drain free");
            ++cycles;
        }
        std::printf("stress: 1000 allocations, %llu frees\n", static_cast<unsigned long long>(cycles));

        GPUAllocation twice = allocator.allocate(4096u, GPUMemoryType::Device);
        GPUAllocation copy = twice;
        expectTrue(allocator.free(twice), "first free");
        expectTrue(!allocator.free(copy), "double free refused");
        expectTrue(cudaGetLastError() == cudaSuccess, "refused double free never reached the CUDA runtime");

        expectTrue(allocator.free(counter), "counter free");
        const fuse::alloc::GPUAllocatorStats stats = allocator.stats();
        for (u32 t = 0; t < fuse::alloc::kGPUMemoryTypeCount; ++t) {
            const fuse::alloc::GPUMemoryTypeStats& s = stats.types[t];
            std::printf("  %-12s allocations %llu frees %llu failed %llu peak %.1f MiB\n",
                        fuse::alloc::gpu_memory_type_name(static_cast<GPUMemoryType>(t)),
                        static_cast<unsigned long long>(s.allocations), static_cast<unsigned long long>(s.frees),
                        static_cast<unsigned long long>(s.failed), static_cast<double>(s.peak_live_bytes) / 1048576.0);
            expectTrue(s.allocations == s.frees && s.live_count == 0u && s.live_bytes == 0u && s.failed == 0u,
                       "per-type allocations == frees, nothing live, no failures");
        }
        expectTrue(stats.invalid_frees == 1u, "exactly the deliberate double free was refused");
    }
    (void)cudaOk(cudaDeviceSynchronize(), "final sync");
    size_t freeEnd = 0;
    (void)cudaOk(cudaMemGetInfo(&freeEnd, &total), "cudaMemGetInfo (end)");
    const long long delta = static_cast<long long>(freeStart) - static_cast<long long>(freeEnd);
    std::printf("device free memory: start %.1f MiB, end %.1f MiB (delta %lld bytes)\n",
                static_cast<double>(freeStart) / 1048576.0, static_cast<double>(freeEnd) / 1048576.0, delta);
    // The runtime may keep a few pool pages; anything near an allocation size is a leak.
    expectTrue(delta < static_cast<long long>(4u << 20), "no device memory leaked (free bytes back within 4 MiB)");

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_gpu_allocator_device_gate: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_gpu_allocator_device_gate: Device, Pinned, Managed, DeviceMapped allocate/free verified\n");
    return EXIT_SUCCESS;
}
