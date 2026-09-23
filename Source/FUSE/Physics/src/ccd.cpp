#include <fuse/physics/ccd/ccd.hpp>
#include <fuse/physics/ccd/toi_buffer.hpp>
#include <fuse/physics/narrowphase/collision_dispatch.hpp>
#include <fuse/physics/rotation.hpp>

namespace fuse::physics {

namespace {

CollisionShapeType shapeType(const CollisionShapeSoA& shapes, u32 shapeIndex) {
    if (shapeIndex >= shapes.count()) {
        return CollisionShapeType::Sphere;
    }
    return static_cast<CollisionShapeType>(shapes.types[shapeIndex]);
}

u32 findShapeForBody(const CollisionShapeSoA& shapes, u32 bodyIndex, CollisionShapeType preferred) {
    // Common layout: one shape per body, added in body order. Multi-shape bodies (adjacent
    // shapes of the same body) take the scan below so `preferred` still wins.
    if (bodyIndex < shapes.count() && shapes.bodyIndices[bodyIndex] == bodyIndex) {
        const bool soleShape = (bodyIndex == 0u || shapes.bodyIndices[bodyIndex - 1u] != bodyIndex) &&
                               (bodyIndex + 1u >= shapes.count() || shapes.bodyIndices[bodyIndex + 1u] != bodyIndex);
        if (soleShape || static_cast<CollisionShapeType>(shapes.types[bodyIndex]) == preferred) {
            return bodyIndex;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex &&
            static_cast<CollisionShapeType>(shapes.types[i]) == preferred) {
            return i;
        }
    }
    for (u32 i = 0; i < shapes.count(); ++i) {
        if (shapes.bodyIndices[i] == bodyIndex) {
            return i;
        }
    }
    return shapes.count();
}

bool bodyNeedsCcd(const RigidBodySoA& bodies, u32 bodyIndex) {
    return bodyIndex < bodies.count() && (bodies.flags[bodyIndex] & RB_CCD) != 0u;
}

/// A shape as an oriented box "core" (degenerate for spheres and capsules) swept by a ball of
/// `radius`: the rotation-dependent part is the core alone.
struct CcdCore {
    vec3 centre{};
    vec3 axis[3]{};
    f32 extent[3]{};
    f32 radius = 0.f;
};

CcdCore makeCore(CollisionShapeType type, vec3 params, vec3 position, const quat& orientation) {
    CcdCore core{};
    core.centre = position;
    core.axis[0] = rotate(orientation, {1.f, 0.f, 0.f});
    core.axis[1] = rotate(orientation, {0.f, 1.f, 0.f});
    core.axis[2] = rotate(orientation, {0.f, 0.f, 1.f});
    switch (type) {
    case CollisionShapeType::Box:
        core.extent[0] = params.x;
        core.extent[1] = params.y;
        core.extent[2] = params.z;
        break;
    case CollisionShapeType::Capsule:
        core.extent[1] = params.y;
        core.radius = params.x;
        break;
    default:
        core.radius = params.x;
        break;
    }
    return core;
}

/// Distance from the centre to the furthest point of the core (what rotation can move).
f32 coreReach(CollisionShapeType type, vec3 params) {
    switch (type) {
    case CollisionShapeType::Box:
        return params.length();
    case CollisionShapeType::Capsule:
        return params.y;
    default:
        return 0.f;
    }
}

f32 projectedCoreRadius(const CcdCore& core, vec3 axis) {
    return std::fabs(core.axis[0].dot(axis)) * core.extent[0] + std::fabs(core.axis[1].dot(axis)) * core.extent[1] +
           std::fabs(core.axis[2].dot(axis)) * core.extent[2];
}

bool isPointCore(const CcdCore& core) {
    return core.extent[0] == 0.f && core.extent[1] == 0.f && core.extent[2] == 0.f;
}

bool isSegmentCore(const CcdCore& core) {
    return core.extent[0] == 0.f && core.extent[2] == 0.f;
}

/// Lower bound on the distance between two cores (exact for point/segment pairs and point-box;
/// the separating-axis bound over the 15 box axes otherwise). Negative when they overlap.
f32 coreSeparationLowerBound(const CcdCore& a, const CcdCore& b) {
    if (isSegmentCore(a) && isSegmentCore(b)) {
        const vec3 ha = a.axis[1] * a.extent[1];
        const vec3 hb = b.axis[1] * b.extent[1];
        vec3 onA{};
        vec3 onB{};
        narrowphase::closestPointsSegmentSegment(a.centre - ha, a.centre + ha, b.centre - hb, b.centre + hb, onA, onB);
        return (onA - onB).length();
    }
    if (isPointCore(a) || isPointCore(b)) {
        const CcdCore& point = isPointCore(a) ? a : b;
        const CcdCore& box = isPointCore(a) ? b : a;
        const vec3 d = point.centre - box.centre;
        vec3 outside{};
        f32 inside = -1e30f;
        for (u32 k = 0; k < 3u; ++k) {
            const f32 coordinate = d.dot(box.axis[k]);
            const f32 excess = std::fabs(coordinate) - box.extent[k];
            inside = std::max(inside, excess);
            if (excess > 0.f) {
                outside += box.axis[k] * excess;
            }
        }
        return inside > 0.f ? outside.length() : inside;
    }
    const vec3 d = a.centre - b.centre;
    f32 separation = -1e30f;
    const auto test = [&](vec3 axis) {
        separation = std::max(separation,
                              std::fabs(d.dot(axis)) - projectedCoreRadius(a, axis) - projectedCoreRadius(b, axis));
    };
    for (u32 i = 0; i < 3u; ++i) {
        test(a.axis[i]);
        test(b.axis[i]);
    }
    for (u32 i = 0; i < 3u; ++i) {
        for (u32 j = 0; j < 3u; ++j) {
            const vec3 axis = a.axis[i].cross(b.axis[j]);
            const f32 length = axis.length();
            if (length > 1e-4f) {
                test(axis * (1.f / length));
            }
        }
    }
    return separation;
}

/// Signed distance from a plane to the lowest point of a core (minus its radius).
f32 corePlaneSeparation(const CcdCore& core, vec3 normal, f32 planeDistance) {
    return core.centre.dot(normal) - planeDistance - projectedCoreRadius(core, normal) - core.radius;
}

f32 shapeSeparationLowerBound(const narrowphase::ShapeInstance& a, const narrowphase::ShapeInstance& b) {
    if (b.type == CollisionShapeType::Plane) {
        return corePlaneSeparation(makeCore(a.type, a.params, a.position, a.orientation), b.params, b.scalar);
    }
    if (a.type == CollisionShapeType::Plane) {
        return corePlaneSeparation(makeCore(b.type, b.params, b.position, b.orientation), a.params, a.scalar);
    }
    const CcdCore coreA = makeCore(a.type, a.params, a.position, a.orientation);
    const CcdCore coreB = makeCore(b.type, b.params, b.position, b.orientation);
    return coreSeparationLowerBound(coreA, coreB) - coreA.radius - coreB.radius;
}

} // namespace

TOIResult conservativeAdvancementToi(const narrowphase::ShapeInstance& shapeA,
                                     vec3 displacementA,
                                     vec3 rotationA,
                                     const narrowphase::ShapeInstance& shapeB,
                                     vec3 displacementB,
                                     vec3 rotationB,
                                     f32 tolerance) {
    // Any surface point moves at most |dx| + |theta| r_core over the frame, so after measuring a
    // separation d the shapes cannot touch before t + d / bound.
    const f32 bound = (displacementA - displacementB).length() + rotationA.length() * coreReach(shapeA.type, shapeA.params) +
                      rotationB.length() * coreReach(shapeB.type, shapeB.params);
    const auto poseAt = [](const narrowphase::ShapeInstance& shape, vec3 displacement, vec3 rotation, f32 t) {
        narrowphase::ShapeInstance moved = shape;
        moved.position = shape.position + displacement * t;
        moved.orientation = applyRotationVector(shape.orientation, rotation * t);
        return moved;
    };
    f32 t = 0.f;
    bool converged = false;
    narrowphase::ShapeInstance a = shapeA;
    narrowphase::ShapeInstance b = shapeB;
    for (u32 iteration = 0; iteration < kCcdMaxAdvancementSteps; ++iteration) {
        a = poseAt(shapeA, displacementA, rotationA, t);
        b = poseAt(shapeB, displacementB, rotationB, t);
        const f32 separation = shapeSeparationLowerBound(a, b);
        if (separation <= tolerance) {
            converged = true;
            break;
        }
        if (bound <= 1e-9f) {
            return {.valid = false};
        }
        t += (separation - 0.5f * tolerance) / bound;
        if (t > 1.f) {
            return {.valid = false};
        }
    }
    (void)converged; // out of steps: t is still a safe (conservative) time to stop at
    TOIResult result{};
    result.toi = t;
    result.valid = true;
    const narrowphase::ContactManifold manifold = narrowphase::collideShapes(a, b, 0u, 1u, 4.f * tolerance + 1e-3f);
    if (manifold.valid && manifold.pointCount > 0u) {
        result.contactNormal = manifold.contactNormal;
        u32 deepest = 0;
        for (u32 k = 1; k < manifold.pointCount; ++k) {
            deepest = manifold.points[k].penetration > manifold.points[deepest].penetration ? k : deepest;
        }
        result.contactPoint = manifold.points[deepest].point;
    } else if (b.type == CollisionShapeType::Plane) {
        result.contactNormal = b.params;
        result.contactPoint = a.position;
    } else if (a.type == CollisionShapeType::Plane) {
        result.contactNormal = a.params * -1.f;
        result.contactPoint = b.position;
    } else {
        result.contactNormal = (a.position - b.position).normalized();
        result.contactPoint = (a.position + b.position) * 0.5f;
    }
    return result;
}

namespace {

TOIResult dispatchCcdPair(const broadphase::CandidatePair& pair,
                          const RigidBodySoA& bodies,
                          const CollisionShapeSoA& shapes,
                          f32 dt) {
    if (pair.bodyA >= bodies.count() || pair.bodyB >= bodies.count() || dt <= 0.f) {
        return {};
    }

    if (!bodyNeedsCcd(bodies, pair.bodyA) && !bodyNeedsCcd(bodies, pair.bodyB)) {
        return {};
    }

    const u32 shapeA = findShapeForBody(shapes, pair.bodyA, CollisionShapeType::Sphere);
    const u32 shapeB = findShapeForBody(shapes, pair.bodyB, CollisionShapeType::Sphere);
    if (shapeA >= shapes.count() || shapeB >= shapes.count()) {
        return {};
    }

    const CollisionShapeType typeA = shapeType(shapes, shapeA);
    const CollisionShapeType typeB = shapeType(shapes, shapeB);
    const vec3 posA = bodies.positions[pair.bodyA];
    const vec3 posB = bodies.positions[pair.bodyB];
    const vec3 velA = bodies.linearVelocities[pair.bodyA] * dt;
    const vec3 velB = bodies.linearVelocities[pair.bodyB] * dt;

    // Rotation matters only for the non-spherical cores: oriented or spinning boxes and capsules
    // (and every pair without a closed form) take conservative advancement.
    const vec3 spinA = bodies.angularVelocities[pair.bodyA] * dt;
    const vec3 spinB = bodies.angularVelocities[pair.bodyB] * dt;
    const auto needsSweep = [&](CollisionShapeType type, u32 shapeIndex, u32 body, vec3 spin) {
        if (type == CollisionShapeType::Sphere || type == CollisionShapeType::Plane) {
            return false;
        }
        return !isIdentity(bodies.orientations[body]) ||
               spin.length() * coreReach(type, shapes.params[shapeIndex]) > kCcdRotationEpsilon;
    };
    // Closed forms: sphere-sphere, sphere-plane and sphere vs an axis-aligned, non-spinning box.
    const bool sphereOrPlaneOnly = typeA != CollisionShapeType::Capsule && typeB != CollisionShapeType::Capsule;
    const bool analytic = (typeA == CollisionShapeType::Sphere || typeB == CollisionShapeType::Sphere) &&
                          sphereOrPlaneOnly && !needsSweep(typeA, shapeA, pair.bodyA, spinA) &&
                          !needsSweep(typeB, shapeB, pair.bodyB, spinB);
    const auto sweepable = [](CollisionShapeType type) {
        return type == CollisionShapeType::Sphere || type == CollisionShapeType::Box ||
               type == CollisionShapeType::Capsule || type == CollisionShapeType::Plane;
    };
    if (!analytic && (!sweepable(typeA) || !sweepable(typeB) ||
                      (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Plane))) {
        return {};
    }
    if (!analytic) {
        const narrowphase::ShapeInstance a{typeA, shapes.params[shapeA], shapes.scalars[shapeA], posA,
                                           bodies.orientations[pair.bodyA]};
        const narrowphase::ShapeInstance b{typeB, shapes.params[shapeB], shapes.scalars[shapeB], posB,
                                           bodies.orientations[pair.bodyB]};
        TOIResult result = conservativeAdvancementToi(a, velA, spinA, b, velB, spinB, kCcdTolerance);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSphereSphere(
            posA, velA, shapes.params[shapeA].x, posB, velB, shapes.params[shapeB].x);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Plane) {
        TOIResult result = sweptSpherePlane(
            posA, velA - velB, shapes.params[shapeA].x, shapes.params[shapeB], shapes.scalars[shapeB]);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Plane && typeB == CollisionShapeType::Sphere) {
        TOIResult result = sweptSpherePlane(
            posB, velB - velA, shapes.params[shapeB].x, shapes.params[shapeA], shapes.scalars[shapeA]);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Sphere && typeB == CollisionShapeType::Box) {
        const aabb box = makeCenteredAabb(posB, shapes.params[shapeB]);
        TOIResult result =
            sweptSphereAabb(posA, velA - velB, shapes.params[shapeA].x, box);
        if (!result.valid) {
            return {};
        }
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    if (typeA == CollisionShapeType::Box && typeB == CollisionShapeType::Sphere) {
        const aabb box = makeCenteredAabb(posA, shapes.params[shapeA]);
        TOIResult result =
            sweptSphereAabb(posB, velB - velA, shapes.params[shapeB].x, box);
        if (!result.valid) {
            return {};
        }
        result.contactNormal = result.contactNormal * -1.f;
        result.bodyA = pair.bodyA;
        result.bodyB = pair.bodyB;
        return result;
    }

    return {};
}

} // namespace

void runCcdIntoBuffer(const std::vector<broadphase::CandidatePair>& pairs,
                      const RigidBodySoA& bodies,
                      const CollisionShapeSoA& shapes,
                      f32 dt,
                      ToiBufferSoA& buffer) {
    const u32 pairCount = static_cast<u32>(pairs.size());
    if (pairCount == 0u) {
        buffer.clear();
        return;
    }

    buffer.preparePairSlots(pairCount);

    // Per-pair slots are job-safe (disjoint writes). Serial dispatch on the CPU stub avoids
    // scheduler reference-capture flakes; the slot layout matches the future parallel_for path.
    for (u32 pairIndex = 0; pairIndex < pairCount; ++pairIndex) {
        const TOIResult result = dispatchCcdPair(pairs[pairIndex], bodies, shapes, dt);
        if (result.valid) {
            buffer.writeSlot(pairIndex, result);
        }
    }

    buffer.compactAndSort();
}

u32 CcdPipeline::sweepPairs(const RigidBodySoA& bodies,
                            const CollisionShapeSoA& shapes,
                            const std::vector<broadphase::CandidatePair>& pairs,
                            f32 dt,
                            std::vector<TOIResult>& outResults) const {
    ToiBufferSoA buffer;
    buffer.reserve(static_cast<u32>(pairs.size()));
    runCcdIntoBuffer(pairs, bodies, shapes, dt, buffer);
    outResults = buffer.toVector();
    lastResultCount_ = buffer.activeCount;
    return lastResultCount_;
}

} // namespace fuse::physics
