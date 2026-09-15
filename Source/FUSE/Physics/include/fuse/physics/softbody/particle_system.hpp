#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/types.hpp>

namespace fuse::physics {

/// GPU-resident particle arrays (SoA) — host stub until CUDA B4.8 kernels land.
struct ParticleSoA {
    vec3* positions = nullptr;
    vec3* prevPositions = nullptr;
    vec3* velocities = nullptr;
    f32* invMasses = nullptr;
    u32 count = 0;
    u32 capacity = 0;

    static ParticleSoA allocate(u32 capacity);
    static void free(ParticleSoA& particles);
};

struct ParticleConstraint {
    u32 a = 0;
    u32 b = 0;
    f32 restLength = 0.f;
    f32 compliance = 0.f;
    f32 stiffness = 1.f;
};

} // namespace fuse::physics
