#include <fuse/physics/softbody/particle_system.hpp>

#include <new>

namespace fuse::physics {

ParticleSoA ParticleSoA::allocate(u32 capacity) {
    ParticleSoA particles{};
    if (capacity == 0) {
        return particles;
    }

    particles.positions = new vec3[capacity];
    particles.prevPositions = new vec3[capacity];
    particles.velocities = new vec3[capacity];
    particles.invMasses = new f32[capacity];
    particles.count = 0;
    particles.capacity = capacity;
    return particles;
}

void ParticleSoA::free(ParticleSoA& particles) {
    delete[] particles.positions;
    delete[] particles.prevPositions;
    delete[] particles.velocities;
    delete[] particles.invMasses;
    particles = {};
}

} // namespace fuse::physics
