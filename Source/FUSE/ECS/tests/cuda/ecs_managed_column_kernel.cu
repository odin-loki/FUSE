// Device side of fuse_b3_ecs_managed_column_device (see test_ecs_managed_column_device.cpp): the kernel
// reads Transform rows in place from an ECS column that lives in CUDA managed memory and assert()s on the
// device that each row holds the gate position of its entity (ecs_managed_column_check.hpp — the same
// check the CPU gate runs). NDEBUG is undefined here so the device assert is compiled in every config.

#undef NDEBUG
#include <cassert>

#include "../ecs_managed_column_check.hpp"

#include <fuse/ecs/components/transform.hpp>

#include <cuda_runtime.h>

#include <cstddef>

namespace {

using fuse::u32;
namespace ecs = fuse::ecs;
namespace gate = fuse::ecs_gate;

struct DeviceTally {
    unsigned long long rows;
    unsigned long long matches;
    double checksum;
};

__global__ void readManagedTransforms(const ecs::Transform* column, const u32* entityIndex, u32 rows,
                                      unsigned int hostTransformSize, unsigned int hostPositionOffset,
                                      DeviceTally* tally) {
    // Host (C++23 TU) and device agree on the component layout the kernel reads.
    assert(sizeof(ecs::Transform) == hostTransformSize);
    assert(offsetof(ecs::Transform, position) == hostPositionOffset);
    const u32 row = blockIdx.x * blockDim.x + threadIdx.x;
    if (row >= rows) {
        return;
    }
    const ecs::Transform& t = column[row];
    const bool match = gate::positionMatches(t, entityIndex[row]);
    assert(match && "Transform position read from the managed ECS column does not match the gate scene");
    atomicAdd(&tally->rows, 1ull);
    atomicAdd(&tally->matches, match ? 1ull : 0ull);
    atomicAdd(&tally->checksum, gate::positionChecksum(t));
}

} // namespace

/// Launches the check over one archetype chunk and synchronises (host code must not touch managed memory
/// while a kernel runs on devices without concurrent managed access, e.g. under Windows WDDM).
/// `tally` points at managed memory laid out as {u64 rows, u64 matches, f64 checksum}.
extern "C" int fuse_ecs_gate_read_managed_transforms(const void* column, const fuse::u32* entityIndex, fuse::u32 rows,
                                                     unsigned int hostTransformSize, unsigned int hostPositionOffset,
                                                     void* tally) {
    if (rows == 0u) {
        return static_cast<int>(cudaSuccess);
    }
    readManagedTransforms<<<(rows + 127u) / 128u, 128>>>(static_cast<const ecs::Transform*>(column), entityIndex, rows,
                                                          hostTransformSize, hostPositionOffset,
                                                          static_cast<DeviceTally*>(tally));
    cudaError_t err = cudaGetLastError();
    if (err == cudaSuccess) {
        err = cudaDeviceSynchronize();
    }
    return static_cast<int>(err);
}
