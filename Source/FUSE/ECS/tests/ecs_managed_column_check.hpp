#pragma once

// B3 row "CUDA kernel reads Transform positions from managed-memory ECS column — verified with device-side
// assert": the single definition of the gate scene's Transform positions and of the per-row check. The CPU
// gate (fuse_b3_ecs_managed_column) runs it over the pmr-backed column on the host; the device gate
// (fuse_b3_ecs_managed_column_device) runs it inside a __global__ kernel over the same column in CUDA
// managed memory and asserts on the device.

#include <fuse/ecs/components/transform.hpp>
#include <fuse/types.hpp>

namespace fuse::ecs_gate {

inline constexpr u32 kGateEntities = 10000;

/// Position the gate gives entity `index` (exactly representable floats, so every backend agrees bit for bit).
FUSE_HOST_DEVICE inline void expectedPosition(u32 index, f32& x, f32& y, f32& z) {
    x = static_cast<f32>(index) * 0.5f + 1.f;
    y = static_cast<f32>(index % 97u) - 48.f;
    z = -static_cast<f32>(index) * 0.25f;
}

/// True when `t` holds the gate position of entity `index` (w = 1, the homogeneous point convention).
FUSE_HOST_DEVICE inline bool positionMatches(const ecs::Transform& t, u32 index) {
    f32 x = 0.f;
    f32 y = 0.f;
    f32 z = 0.f;
    expectedPosition(index, x, y, z);
    return t.position.x == x && t.position.y == y && t.position.z == z && t.position.w == 1.f;
}

/// Order-independent checksum term (exact in f64 for the gate's positions).
FUSE_HOST_DEVICE inline f64 positionChecksum(const ecs::Transform& t) {
    return static_cast<f64>(t.position.x) + 3.0 * static_cast<f64>(t.position.y) + 7.0 * static_cast<f64>(t.position.z);
}

} // namespace fuse::ecs_gate
