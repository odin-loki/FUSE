#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>

namespace fuse::physics::narrowphase {

// The body lives in primitive_contacts.hpp (FUSE_HOST_DEVICE, shared with the CUDA narrowphase).
ContactManifold collideBoxBox(
    vec3 posA,
    vec3 halfExtentsA,
    vec3 posB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin) {
    return hd::collideBoxBox(posA, halfExtentsA, posB, halfExtentsB, idxA, idxB, margin);
}

} // namespace fuse::physics::narrowphase
