// B3 row "CUDA kernel reads Transform positions from managed-memory ECS column — verified with device-side
// assert". Device gate (RTX 3090; exit 77 without a CUDA device or without FUSE_BUILD_CUDA).
//
// The registry's Transform columns are put in CUDA managed memory (Registry::set_column_memory_resource
// with a fuse::alloc::GPUManagedMemoryResource), the 10k-entity gate scene is built through the normal ECS
// API (several archetypes, migrations, swap-removes), and for every archetype chunk a kernel
// (cuda/ecs_managed_column_kernel.cu) reads the Transform rows *in place* through the pointer
// each_chunk<Transform> hands out. Each thread assert()s on the device that its row holds the gate position
// of its entity and that host/device agree on the Transform layout; it also accumulates a checksum the host
// compares exactly.
//
//   (no args)   run the gate; exit 0 when every row matched and no device assert fired
//   --corrupt   change one position on the host first and require the device assert to fire
//               (cudaErrorAssert); exit 0 when it did — proves the assert is compiled in and live
//   --probe     exit 0 with a device, 77 without

#include "ecs_managed_column_check.hpp"

#include <fuse/alloc/gpu_allocator.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/rigidbody.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/registry.hpp>

#if defined(FUSE_HAS_CUDA)
#include <cuda_runtime.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#if defined(FUSE_HAS_CUDA)
extern "C" int fuse_ecs_gate_read_managed_transforms(const void* column, const fuse::u32* entityIndex, fuse::u32 rows,
                                                     unsigned int hostTransformSize, unsigned int hostPositionOffset,
                                                     void* tally);
#endif

namespace {

using fuse::f64;
using fuse::u32;
using fuse::u64;
using fuse::usize;
namespace ecs = fuse::ecs;
namespace gate = fuse::ecs_gate;

#if defined(FUSE_HAS_CUDA)
int g_failures = 0;

void expectTrue(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

struct DeviceTally {
    unsigned long long rows;
    unsigned long long matches;
    double checksum;
};

ecs::Transform gateTransform(u32 index) {
    ecs::Transform t{};
    gate::expectedPosition(index, t.position.x, t.position.y, t.position.z);
    t.position.w = 1.f;
    return t;
}

bool isManaged(const void* p) {
    cudaPointerAttributes attributes{};
    if (cudaPointerGetAttributes(&attributes, p) != cudaSuccess) {
        (void)cudaGetLastError();
        return false;
    }
    return attributes.type == cudaMemoryTypeManaged;
}

/// Runs the kernel over every Transform chunk. Returns the first CUDA error (cudaSuccess when all ran).
cudaError_t readAllChunks(ecs::Registry& registry, u32* entityIndex, DeviceTally* tally, u32& chunks,
                          u32& unmanagedChunks, f64& expected) {
    *tally = DeviceTally{0ull, 0ull, 0.0};
    chunks = 0;
    unmanagedChunks = 0;
    expected = 0.0;
    cudaError_t error = cudaSuccess;
    usize firstRow = 0;
    registry.each_chunk<ecs::Transform>([&](std::span<const ecs::EntityID> ids, std::span<ecs::Transform> column) {
        if (error != cudaSuccess) {
            return;
        }
        ++chunks;
        unmanagedChunks += isManaged(column.data()) ? 0u : 1u;
        // The per-row entity index is host metadata (Archetype::entities); the Transform rows themselves are
        // read by the kernel straight out of the ECS column.
        for (usize row = 0; row < column.size(); ++row) {
            entityIndex[firstRow + row] = ids[row].index;
            expected += gate::positionChecksum(gateTransform(ids[row].index));
        }
        error = static_cast<cudaError_t>(fuse_ecs_gate_read_managed_transforms(
            column.data(), entityIndex + firstRow, static_cast<u32>(column.size()),
            static_cast<unsigned int>(sizeof(ecs::Transform)),
            static_cast<unsigned int>(offsetof(ecs::Transform, position)), tally));
        firstRow += column.size();
    });
    return error;
}
#endif

} // namespace

int main(int argc, char** argv) {
    const bool corrupt = argc > 1 && std::strcmp(argv[1], "--corrupt") == 0;
    const bool probe = argc > 1 && std::strcmp(argv[1], "--probe") == 0;
#if !defined(FUSE_HAS_CUDA)
    (void)corrupt;
    (void)probe;
    std::printf("SKIP fuse_b3_ecs_managed_column_device: built without CUDA (FUSE_BUILD_CUDA=OFF)\n");
    return 77;
#else
    if (!fuse::alloc::GPUAllocator::cuda_available()) {
        std::printf("SKIP fuse_b3_ecs_managed_column_device: %s\n", fuse::alloc::GPUAllocator::unavailable_reason());
        return 77;
    }
    int managed = 0;
    (void)cudaDeviceGetAttribute(&managed, cudaDevAttrManagedMemory, 0);
    if (managed == 0) {
        std::printf("SKIP fuse_b3_ecs_managed_column_device: device 0 has no managed memory support\n");
        return 77;
    }
    if (probe) {
        return 0;
    }

    fuse::alloc::GPUAllocator allocator;
    fuse::alloc::GPUManagedMemoryResource resource(allocator);
    {
        ecs::Registry registry;
        registry.init();
        registry.set_column_memory_resource<ecs::Transform>(&resource);

        std::vector<ecs::EntityID> ids;
        for (u32 i = 0; i < gate::kGateEntities; ++i) {
            const ecs::EntityID id = registry.create();
            ids.push_back(id);
            registry.add<ecs::Transform>(id, gateTransform(id.index));
            if (i % 3u == 0u) {
                registry.add<ecs::Mesh>(id);
            }
            if (i % 5u == 0u) {
                registry.add<ecs::RigidBody>(id);
            }
        }
        for (u32 i = 0; i < gate::kGateEntities; i += 7u) {
            registry.remove<ecs::RigidBody>(ids[i]);
        }
        for (u32 i = 1; i < gate::kGateEntities; i += 11u) {
            registry.destroy_entity(ids[i]);
        }
        if (corrupt) {
            registry.get<ecs::Transform>(ids[42])->position.y += 0.5f;
            std::printf("--corrupt: entity %u position.y changed; the device assert must fire\n", ids[42].index);
        }

        fuse::alloc::GPUAllocation tallyAlloc =
            allocator.allocate(sizeof(DeviceTally), fuse::alloc::GPUMemoryType::Managed);
        fuse::alloc::GPUAllocation indexAlloc =
            allocator.allocate(sizeof(u32) * registry.count(), fuse::alloc::GPUMemoryType::Managed);
        if (!tallyAlloc.valid() || !indexAlloc.valid()) {
            std::fprintf(stderr, "FAIL: managed tally / entity-index buffers (%s)\n", allocator.last_error());
            return EXIT_FAILURE;
        }
        auto* tally = static_cast<DeviceTally*>(tallyAlloc.host_ptr);
        auto* entityIndex = static_cast<u32*>(indexAlloc.host_ptr);

        u32 chunks = 0;
        u32 unmanagedChunks = 0;
        f64 expected = 0.0;
        const cudaError_t error = readAllChunks(registry, entityIndex, tally, chunks, unmanagedChunks, expected);
        if (corrupt) {
            std::printf("device assert %s (%s)\n", error == cudaErrorAssert ? "fired" : "did NOT fire",
                        cudaGetErrorString(error));
            // The context is unusable after a device assert: leave without further CUDA calls.
            std::fflush(stdout);
            std::_Exit(error == cudaErrorAssert ? EXIT_SUCCESS : EXIT_FAILURE);
        }
        if (error != cudaSuccess) {
            std::fprintf(stderr, "FAIL: device-side assert / kernel error: %s\n", cudaGetErrorString(error));
            std::fflush(stderr);
            std::_Exit(EXIT_FAILURE);
        }
        std::printf("managed Transform column: %u archetype chunks, %llu rows read on the device, %llu matched, "
                    "checksum %.3f (expected %.3f)\n",
                    chunks, tally->rows, tally->matches, tally->checksum, expected);
        expectTrue(unmanagedChunks == 0u, "every Transform column pointer is CUDA managed memory");
        expectTrue(chunks >= 3u, "several archetypes carry Transform");
        expectTrue(tally->rows == registry.count() && tally->matches == registry.count(),
                   "every live entity's Transform read and matched on the device");
        expectTrue(tally->checksum == expected, "device checksum == host checksum (exact)");

        // Host writes between kernels go through the same pointers (managed pages migrate back and forth).
        registry.each_chunk<ecs::Transform>([](std::span<const ecs::EntityID>, std::span<ecs::Transform> column) {
            for (ecs::Transform& t : column) {
                t.position.w = 1.f;
            }
        });
        const cudaError_t again = readAllChunks(registry, entityIndex, tally, chunks, unmanagedChunks, expected);
        expectTrue(again == cudaSuccess && tally->matches == registry.count() && tally->checksum == expected,
                   "second pass after host access still matches");

        (void)allocator.free(tallyAlloc);
        (void)allocator.free(indexAlloc);
        registry.destroy();
    }
    const fuse::alloc::GPUAllocatorStats stats = allocator.stats();
    std::printf("managed column blocks: %llu allocated, %llu returned; allocator live %llu\n",
                static_cast<unsigned long long>(resource.allocation_count()),
                static_cast<unsigned long long>(resource.deallocation_count()),
                static_cast<unsigned long long>(stats.live_count()));
    expectTrue(resource.allocation_count() > 0u && resource.allocation_count() == resource.deallocation_count(),
               "every managed column block returned");
    expectTrue(stats.live_count() == 0u && stats.invalid_frees == 0u, "no managed memory leaked");

    if (g_failures != 0) {
        std::fprintf(stderr, "fuse_b3_ecs_managed_column_device: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("fuse_b3_ecs_managed_column_device: CUDA kernel read every Transform from the managed ECS column\n");
    return EXIT_SUCCESS;
#endif
}
