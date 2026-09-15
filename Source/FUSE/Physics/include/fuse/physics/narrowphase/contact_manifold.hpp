#pragma once

#include <fuse/physics/math.hpp>
#include <fuse/physics/narrowphase/friction.hpp>
#include <fuse/types.hpp>

namespace fuse::physics::narrowphase {

constexpr u32 kMaxContactPointsPerManifold = 4u;

struct ContactPointSlot {
    vec3 point{};
    f32 penetration = 0.f;
};

/// Narrowphase contact manifold with multi-point slots and warm-start impulse stubs (B4.3 deepen).
struct ContactManifold {
    ContactManifold() = default;

    vec3 contactNormal{};
    f32 minSeparation = 0.f;
    u32 bodyA = 0;
    u32 bodyB = 0;
    u32 pointCount = 0;
    ContactPointSlot points[kMaxContactPointsPerManifold]{};
    f32 warmNormalImpulse = 0.f;
    vec2 warmTangentImpulse{};
    TangentBasis frictionBasis{};

    // Legacy single-point mirror used by PBD solver and existing tests.
    vec3 contactPoint{};
    f32 penetrationDepth = 0.f;
    bool valid = false;

    void reset();
    void addPoint(vec3 point, f32 penetration);
    void syncLegacyFields();
    void buildFrictionBasis();

    bool empty() const { return pointCount == 0u; }
    bool hasFrictionBasis() const;
    const ContactPointSlot& pointAt(u32 index) const;
    f32 maxPenetration() const;
};

inline ContactManifold invalidContactManifold() {
    return ContactManifold();
}

} // namespace fuse::physics::narrowphase
