#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/narrowphase/primitive_contacts.hpp>

namespace fuse::physics::narrowphase {

// The bodies live in primitive_contacts.hpp (FUSE_HOST_DEVICE, shared with the CUDA narrowphase).

ContactManifold collideOrientedBoxPlane(
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxBox,
    u32 idxPlane,
    f32 margin) {
    return hd::collideOrientedBoxPlane(boxPos, boxRotation, boxHalfExtents, planeNormal, planeDistance, idxBox,
                                       idxPlane, margin);
}

ContactManifold collideOrientedBoxSphere(
    vec3 spherePos,
    f32 sphereRadius,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxSphere,
    u32 idxBox,
    f32 margin) {
    return hd::collideOrientedBoxSphere(spherePos, sphereRadius, boxPos, boxRotation, boxHalfExtents, idxSphere,
                                        idxBox, margin);
}

ContactManifold collideOrientedBoxBox(
    vec3 posA,
    quat rotationA,
    vec3 halfExtentsA,
    vec3 posB,
    quat rotationB,
    vec3 halfExtentsB,
    u32 idxA,
    u32 idxB,
    f32 margin) {
    return hd::collideOrientedBoxBox(posA, rotationA, halfExtentsA, posB, rotationB, halfExtentsB, idxA, idxB,
                                     margin);
}

ContactManifold collideCapsulePlane(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 planeNormal,
    f32 planeDistance,
    u32 idxCapsule,
    u32 idxPlane,
    f32 margin) {
    return hd::collideCapsulePlane(capsulePos, capsuleRotation, capsuleParams, planeNormal, planeDistance,
                                   idxCapsule, idxPlane, margin);
}

ContactManifold collideCapsuleBox(
    vec3 capsulePos,
    quat capsuleRotation,
    vec3 capsuleParams,
    vec3 boxPos,
    quat boxRotation,
    vec3 boxHalfExtents,
    u32 idxCapsule,
    u32 idxBox,
    f32 margin) {
    return hd::collideCapsuleBox(capsulePos, capsuleRotation, capsuleParams, boxPos, boxRotation, boxHalfExtents,
                                 idxCapsule, idxBox, margin);
}

} // namespace fuse::physics::narrowphase
