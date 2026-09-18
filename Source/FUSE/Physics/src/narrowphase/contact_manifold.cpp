#include <fuse/physics/narrowphase/contact_manifold.hpp>

#include <algorithm>
#include <cmath>

namespace fuse::physics::narrowphase {

TangentBasis ContactPoint::tangent_basis(vec3 contactNormal) const {
    return buildTangentBasis(contactNormal);
}

void ContactManifold::reset() {
    contactNormal = {};
    minSeparation = 0.f;
    bodyA = 0;
    bodyB = 0;
    pointCount = 0;
    warmNormalImpulse = 0.f;
    warmTangentImpulse = {};
    frictionBasis = {};
    contactPoint = {};
    penetrationDepth = 0.f;
    valid = false;
    for (u32 i = 0u; i < kMaxContactPointsPerManifold; ++i) {
        points[i] = {};
    }
}

void ContactManifold::clear() {
    reset();
}

void ContactManifold::buildFrictionBasis() {
    if (contactNormal.length() < 1e-8f) {
        frictionBasis = {};
        return;
    }
    frictionBasis = buildTangentBasis(contactNormal);
}

bool ContactManifold::hasValidNormal(f32 epsilon) const {
    return contactNormal.length() > epsilon;
}

bool ContactManifold::hasFrictionBasis() const {
    if (!hasValidNormal()) {
        return false;
    }
    return isOrthonormalTangentBasis(contactNormal, frictionBasis);
}

bool ContactManifold::hasPenetratingPoints(f32 epsilon) const {
    return countPenetratingPoints(epsilon) > 0u;
}

u32 ContactManifold::countPenetratingPoints(f32 epsilon) const {
    u32 penetrating = 0u;
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration >= -epsilon) {
            ++penetrating;
        }
    }
    return penetrating;
}

bool ContactManifold::hasSeparatedPoints(f32 epsilon) const {
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration < -epsilon) {
            return true;
        }
    }
    return false;
}

bool ContactManifold::hasDuplicatePoints(f32 positionEpsilon) const {
    if (pointCount <= 1u) {
        return false;
    }

    const f32 epsilonSq = positionEpsilon * positionEpsilon;
    for (u32 i = 0u; i < pointCount; ++i) {
        for (u32 j = i + 1u; j < pointCount; ++j) {
            const vec3 delta = points[i].point - points[j].point;
            if (delta.dot(delta) <= epsilonSq) {
                return true;
            }
        }
    }
    return false;
}

bool ContactManifold::wouldBeEmptyAfterPrune(
    f32 separationEpsilon,
    f32 duplicateEpsilon) const {
    if (pointCount == 0u) {
        return true;
    }

    ContactPoint surviving[kMaxContactPointsPerManifold]{};
    u32 survivingCount = 0u;
    const f32 epsilonSq = duplicateEpsilon * duplicateEpsilon;

    for (u32 readIndex = 0u; readIndex < pointCount; ++readIndex) {
        if (points[readIndex].penetration < -separationEpsilon) {
            continue;
        }

        bool duplicate = false;
        for (u32 existing = 0u; existing < survivingCount; ++existing) {
            const vec3 delta = points[readIndex].point - surviving[existing].point;
            if (delta.dot(delta) <= epsilonSq) {
                if (points[readIndex].penetration > surviving[existing].penetration) {
                    surviving[existing] = points[readIndex];
                }
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        if (survivingCount < kMaxContactPointsPerManifold) {
            surviving[survivingCount] = points[readIndex];
            ++survivingCount;
        }
    }

    if (survivingCount > kMaxContactPointsPerManifold) {
        survivingCount = kMaxContactPointsPerManifold;
    }

    return survivingCount == 0u;
}

void ContactManifold::invalidateIfEmpty() {
    if (empty()) {
        valid = false;
    }
}

bool ContactManifold::needsPruning(f32 separationEpsilon, f32 duplicateEpsilon) const {
    if (pointCount == 0u) {
        return false;
    }

    u32 penetrating = 0u;
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration < -separationEpsilon) {
            return true;
        }
        if (points[i].penetration >= -separationEpsilon) {
            ++penetrating;
        }
    }

    if (penetrating > kMaxContactPointsPerManifold) {
        return true;
    }

    const f32 epsilonSq = duplicateEpsilon * duplicateEpsilon;
    for (u32 i = 0u; i < pointCount; ++i) {
        for (u32 j = i + 1u; j < pointCount; ++j) {
            const vec3 delta = points[i].point - points[j].point;
            if (delta.dot(delta) <= epsilonSq) {
                return true;
            }
        }
    }

    return false;
}

void ContactManifold::pruneNonPenetratingPoints(f32 epsilon) {
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < pointCount; ++readIndex) {
        if (points[readIndex].penetration < -epsilon) {
            continue;
        }
        if (writeIndex != readIndex) {
            points[writeIndex] = points[readIndex];
        }
        ++writeIndex;
    }

    for (u32 i = writeIndex; i < pointCount; ++i) {
        points[i] = {};
    }
    pointCount = writeIndex;
    syncLegacyFields();
}

void ContactManifold::pruneShallowPenetrations(f32 minDepth) {
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < pointCount; ++readIndex) {
        if (points[readIndex].penetration < minDepth) {
            continue;
        }
        if (writeIndex != readIndex) {
            points[writeIndex] = points[readIndex];
        }
        ++writeIndex;
    }

    for (u32 i = writeIndex; i < pointCount; ++i) {
        points[i] = {};
    }
    pointCount = writeIndex;
    syncLegacyFields();
}

void ContactManifold::pruneToMaxPoints(u32 maxPoints) {
    if (maxPoints == 0u || pointCount <= maxPoints) {
        return;
    }

    u32 keep[kMaxContactPointsPerManifold]{};
    for (u32 i = 0u; i < pointCount; ++i) {
        keep[i] = i;
    }

    std::sort(
        keep,
        keep + pointCount,
        [this](u32 lhs, u32 rhs) { return points[lhs].penetration > points[rhs].penetration; });

    ContactPoint trimmed[kMaxContactPointsPerManifold]{};
    for (u32 i = 0u; i < maxPoints; ++i) {
        trimmed[i] = points[keep[i]];
    }

    for (u32 i = 0u; i < kMaxContactPointsPerManifold; ++i) {
        points[i] = trimmed[i];
    }
    pointCount = maxPoints;
    syncLegacyFields();
}

void ContactManifold::pruneDuplicatePoints(f32 positionEpsilon) {
    if (pointCount <= 1u) {
        return;
    }

    const f32 epsilonSq = positionEpsilon * positionEpsilon;
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < pointCount; ++readIndex) {
        bool duplicate = false;
        for (u32 existing = 0u; existing < writeIndex; ++existing) {
            const vec3 delta = points[readIndex].point - points[existing].point;
            if (delta.dot(delta) <= epsilonSq) {
                if (points[readIndex].penetration > points[existing].penetration) {
                    points[existing] = points[readIndex];
                }
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        if (writeIndex != readIndex) {
            points[writeIndex] = points[readIndex];
        }
        ++writeIndex;
    }

    for (u32 i = writeIndex; i < pointCount; ++i) {
        points[i] = {};
    }
    pointCount = writeIndex;
    syncLegacyFields();
}

void ContactManifold::pruneContactPoints(f32 separationEpsilon, f32 duplicateEpsilon) {
    pruneNonPenetratingPoints(separationEpsilon);
    pruneDuplicatePoints(duplicateEpsilon);
    pruneToMaxPoints(kMaxContactPointsPerManifold);
}

bool ContactManifold::pruneIfEmpty(f32 separationEpsilon, f32 duplicateEpsilon) {
    pruneContactPoints(separationEpsilon, duplicateEpsilon);
    return !empty();
}

bool ContactManifold::canSkipPruneNonPenetrating(f32 epsilon) const {
    return !hasSeparatedPoints(epsilon);
}

bool ContactManifold::canSkipPruneDuplicates(f32 positionEpsilon) const {
    return !hasDuplicatePoints(positionEpsilon);
}

bool ContactManifold::canSkipPruneToMaxPoints(u32 maxPoints) const {
    return pointCount <= maxPoints;
}

bool ContactManifold::canSkipPruneContactPoints(f32 separationEpsilon, f32 duplicateEpsilon) const {
    return !needsPruning(separationEpsilon, duplicateEpsilon);
}

bool ContactManifold::hasShallowPenetrations(f32 minDepth) const {
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration >= -1e-6f && points[i].penetration < minDepth) {
            return true;
        }
    }
    return false;
}

bool ContactManifold::pruneContactPointsIfNeeded(f32 separationEpsilon, f32 duplicateEpsilon) {
    if (canSkipPruneContactPoints(separationEpsilon, duplicateEpsilon)) {
        return !empty();
    }
    return pruneIfEmpty(separationEpsilon, duplicateEpsilon);
}

bool ContactManifold::canSkipPruneShallowPenetrations(f32 minDepth) const {
    if (minDepth <= 0.f) {
        return true;
    }
    return !hasShallowPenetrations(minDepth);
}

ManifoldPrunePreflight preflight_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPrunePreflight preflight{};
    if (manifold.empty()) {
        preflight.skipped = true;
        preflight.wouldBeEmpty = true;
        return preflight;
    }

    preflight.hasSeparated = manifold.hasSeparatedPoints(separationEpsilon);
    preflight.hasDuplicates = manifold.hasDuplicatePoints(duplicateEpsilon);
    if (shallowMinDepth > 0.f) {
        preflight.hasShallowPenetrations = manifold.hasShallowPenetrations(shallowMinDepth);
    }
    preflight.exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    preflight.wouldBeEmpty = manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    return preflight;
}

ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold) {
    ManifoldFinalizePreflight preflight{};
    if (manifold.empty()) {
        preflight.isEmpty = true;
        preflight.skipped = true;
        return preflight;
    }

    preflight.hasInvalidNormal = !manifold.hasValidNormal();
    preflight.hasNoPenetratingPoints = !manifold.hasPenetratingPoints();
    return preflight;
}

bool should_skip_manifold_finalize(const ContactManifold& manifold) {
    return !preflight_manifold_finalize(manifold).can_finalize();
}

bool can_finalize_with_preflight(const ManifoldFinalizePreflight& preflight) {
    return preflight.can_finalize();
}

const ContactPoint& ContactManifold::pointAt(u32 index) const {
    static const ContactPoint empty{};
    if (index >= pointCount) {
        return empty;
    }
    return points[index];
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
