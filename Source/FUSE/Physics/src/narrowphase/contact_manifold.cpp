#include <fuse/physics/narrowphase/contact_manifold.hpp>

#include <algorithm>

namespace fuse::physics::narrowphase {

void ContactManifold::reset() {
    contactNormal = {};
    minSeparation = 0.f;
    bodyA = 0;
    bodyB = 0;
    pointCount = 0;
    warmNormalImpulse = 0.f;
    warmTangentImpulse = {};
    contactPoint = {};
    penetrationDepth = 0.f;
    valid = false;
    for (u32 i = 0u; i < kMaxContactPointsPerManifold; ++i) {
        points[i] = {};
    }
}

f32 ContactManifold::maxPenetration() const {
    if (pointCount == 0u) {
        return 0.f;
    }

    f32 maxPenetration = points[0].penetration;
    for (u32 i = 1u; i < pointCount; ++i) {
        maxPenetration = std::max(maxPenetration, points[i].penetration);
    }
    return maxPenetration;
}

void ContactManifold::addPoint(vec3 point, f32 penetration) {
    if (pointCount >= kMaxContactPointsPerManifold) {
        return;
    }

    points[pointCount].point = point;
    points[pointCount].penetration = penetration;
    ++pointCount;
    syncLegacyFields();
}

void ContactManifold::syncLegacyFields() {
    if (pointCount == 0u) {
        contactPoint = {};
        penetrationDepth = 0.f;
        return;
    }

    u32 dominantIndex = 0u;
    f32 maxPenetration = points[0].penetration;
    for (u32 i = 1u; i < pointCount; ++i) {
        if (points[i].penetration > maxPenetration) {
            maxPenetration = points[i].penetration;
            dominantIndex = i;
        }
    }

    contactPoint = points[dominantIndex].point;
    penetrationDepth = maxPenetration;
}

} // namespace fuse::physics::narrowphase
