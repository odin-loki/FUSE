#include <fuse/physics/narrowphase/contact_manifold.hpp>

#include <fuse/physics/narrowphase/contact_pair.hpp>

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

bool ContactManifold::hasNonUnitNormal(f32 lengthEpsilon) const {
    if (!hasValidNormal()) {
        return false;
    }
    const f32 normalLength = contactNormal.length();
    return std::fabs(normalLength - 1.f) > lengthEpsilon;
}

bool ContactManifold::needsNormalNormalization(f32 lengthEpsilon) const {
    return hasNonUnitNormal(lengthEpsilon);
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

bool ContactManifold::hasShallowPenetrations(f32 minDepth) const {
    for (u32 i = 0u; i < pointCount; ++i) {
        const f32 penetration = points[i].penetration;
        if (penetration >= 0.f && penetration < minDepth) {
            return true;
        }
    }
    return false;
}

u32 ContactManifold::countSeparatedPoints(f32 epsilon) const {
    u32 separated = 0u;
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration < -epsilon) {
            ++separated;
        }
    }
    return separated;
}

void ContactManifold::pruneContactPointsIfNeeded(f32 separationEpsilon, f32 duplicateEpsilon) {
    if (needsPruning(separationEpsilon, duplicateEpsilon)) {
        pruneContactPoints(separationEpsilon, duplicateEpsilon);
    }
}

bool ContactManifold::pruneAndInvalidateIfEmpty(f32 separationEpsilon, f32 duplicateEpsilon) {
    const bool hasPoints = pruneIfEmpty(separationEpsilon, duplicateEpsilon);
    invalidateIfEmpty();
    return hasPoints;
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

bool ContactManifold::canSkipPruneDuplicates(f32 positionEpsilon) const {
    return !hasDuplicatePoints(positionEpsilon);

bool ContactManifold::canSkipPruneToMaxPoints(u32 maxPoints) const {
    return pointCount <= maxPoints;

bool ContactManifold::canSkipPruneContactPoints(f32 separationEpsilon, f32 duplicateEpsilon) const {
    return !needsPruning(separationEpsilon, duplicateEpsilon);

bool ContactManifold::hasShallowPenetrations(f32 minDepth) const {
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration >= -1e-6f && points[i].penetration < minDepth) {
            return true;
    return false;

bool ContactManifold::pruneContactPointsIfNeeded(f32 separationEpsilon, f32 duplicateEpsilon) {
    if (canSkipPruneContactPoints(separationEpsilon, duplicateEpsilon)) {
    return pruneIfEmpty(separationEpsilon, duplicateEpsilon);

bool ContactManifold::canSkipPruneShallowPenetrations(f32 minDepth) const {
    return !hasShallowPenetrations(minDepth);

bool ContactManifold::pruneShallowPenetrationsIfNeeded(f32 minDepth) {
    if (canSkipPruneShallowPenetrations(minDepth)) {
    pruneShallowPenetrations(minDepth);

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::AllSeparated:
        return "AllSeparated";
    return "Unknown";

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::AllSeparated;
    return ManifoldPruneRejectReason::None;

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

ManifoldPrunePreflight preflight_manifold_prune(
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPrunePreflight preflight{};
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.reason != ManifoldPruneRejectReason::None) {
        preflight.skipped = true;
        preflight.wouldBeEmpty = preflight.reason == ManifoldPruneRejectReason::AllSeparated ||
                                 preflight.reason == ManifoldPruneRejectReason::EmptyManifold;
        return preflight;

    preflight.hasSeparated = manifold.hasSeparatedPoints(separationEpsilon);
    preflight.hasDuplicates = manifold.hasDuplicatePoints(duplicateEpsilon);
    if (shallowMinDepth > 0.f) {
        preflight.hasShallow = manifold.hasShallowPenetrations(shallowMinDepth);
    preflight.exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    preflight.wouldBeEmpty = manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    preflight.needsNormalNormalize = manifold.needsNormalNormalization();

bool should_skip_manifold_prune(
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_skip_prune(shallowMinDepth);

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    case ManifoldFinalizeRejectReason::None:
    case ManifoldFinalizeRejectReason::EmptyManifold:
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::AllSeparatedAfterPrune:
        return "AllSeparatedAfterPrune";

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
        return ManifoldFinalizeRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
        return ManifoldFinalizeRejectReason::AllSeparatedAfterPrune;
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    return ManifoldFinalizeRejectReason::None;

bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

ManifoldFinalizePreflight preflight_manifold_finalize(
    f32 frictionEpsilon) {
    ManifoldFinalizePreflight preflight{};
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.reason != ManifoldFinalizeRejectReason::None) {
        preflight.wouldBeEmptyAfterPrune =
            preflight.reason == ManifoldFinalizeRejectReason::AllSeparatedAfterPrune ||
            preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold;

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.needsPruning = prunePreflight.needs_pruning();
    preflight.wouldBeEmptyAfterPrune = prunePreflight.wouldBeEmpty;

    preflight.needsNormalNormalize = manifold.needsNormalNormalization(frictionEpsilon);
    preflight.canFinalize = true;
    preflight.canReuseFrictionBasis = can_skip_friction_basis_rebuild(manifold, frictionEpsilon);
    preflight.needsFrictionBasis = needs_friction_basis_refresh(manifold, frictionEpsilon);

bool can_skip_manifold_finalize(
    return !preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)
                .can_finalize();

bool prune_contact_manifold_with_preflight(
    ContactManifold& manifold,
    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
        if (preflight.reason == ManifoldPruneRejectReason::AllSeparated) {
            manifold.clear();
    if (preflight.can_skip_prune(shallowMinDepth)) {
        return !manifold.empty();
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);

bool finalize_contact_manifold_with_preflight(
    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (!preflight.can_finalize()) {
    if (preflight.needsPruning) {
        if (!prune_contact_manifold_with_preflight(
                manifold, separationEpsilon, duplicateEpsilon)) {
    return generate_contact_manifold(manifold);

bool should_run_manifold_prune(
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

bool should_run_manifold_finalize(
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

bool can_prune_manifold_in_place(
        .can_prune_in_place();
void ContactManifold::pruneShallowPenetrationPoints(f32 minPenetration) {
    u32 writeIndex = 0u;
    for (u32 readIndex = 0u; readIndex < pointCount; ++readIndex) {
        if (points[readIndex].penetration < minPenetration) {
            continue;
        if (writeIndex != readIndex) {
            points[writeIndex] = points[readIndex];
        ++writeIndex;

    for (u32 i = writeIndex; i < pointCount; ++i) {
        points[i] = {};
    pointCount = writeIndex;
    syncLegacyFields();

bool ContactManifold::pruneAndRetainPenetrating(f32 separationEpsilon, f32 duplicateEpsilon) {
    if (empty()) {
        clearWarmStartIfEmpty();

void ContactManifold::clearWarmStartIfEmpty() {
    if (!empty()) {
        return;
    warmNormalImpulse = 0.f;
    warmTangentImpulse = {};
    frictionBasis = {};
    valid = false;

namespace {

bool hasDuplicatePoints(
    if (manifold.pointCount <= 1u) {

    const f32 epsilonSq = duplicateEpsilon * duplicateEpsilon;
    for (u32 i = 0u; i < manifold.pointCount; ++i) {
        for (u32 j = i + 1u; j < manifold.pointCount; ++j) {
            const vec3 delta = manifold.points[i].point - manifold.points[j].point;
            if (delta.dot(delta) <= epsilonSq) {

} // namespace

bool manifold_needs_prune(
    u32 maxPoints,
    f32 shallowPenetration) {
    if (manifold.pointCount == 0u) {
    if (manifold.pointCount > maxPoints) {
    if (hasDuplicatePoints(manifold, duplicateEpsilon)) {

        const f32 penetration = manifold.points[i].penetration;
        if (penetration < -separationEpsilon) {
        if (penetration < shallowPenetration) {
u32 ContactManifold::countSeparatedPoints(f32 epsilon) const {
    u32 separated = 0u;
        if (points[i].penetration < -epsilon) {
            ++separated;
        }
    return separated;

bool ContactManifold::hasUnitNormal(f32 epsilon) const {
    if (!hasValidNormal()) {
    return std::fabs(contactNormal.length() - 1.f) <= epsilon;

bool ContactManifold::normalizeContactNormal(f32 epsilon) {
    if (!hasValidNormal(epsilon)) {
    const f32 normalLength = contactNormal.length();
    contactNormal = contactNormal * (1.f / normalLength);

bool ContactManifold::canFinalize(f32 separationEpsilon, f32 /*duplicateEpsilon*/) const {
    if (empty() || !hasValidNormal()) {
    return countPenetratingPoints(separationEpsilon) > 0u;




bool ContactManifold::pruneForFinalization(f32 separationEpsilon, f32 duplicateEpsilon) {
    pruneContactPoints(separationEpsilon, duplicateEpsilon);
    return hasPenetratingPoints(separationEpsilon);


    if (normalLength < epsilon) {

bool ContactManifold::canFinalize(f32 separationEpsilon, f32 duplicateEpsilon) const {
    if (wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {

    return !empty();
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

u32 ContactManifold::penetratingPointCount(f32 epsilon) const {
    u32 count = 0u;
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration >= -epsilon) {
            ++count;
        }
    }
    return count;
}

u32 ContactManifold::separatedPointCount(f32 epsilon) const {
    u32 count = 0u;
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration < -epsilon) {
            ++count;
        }
    }
    return count;
}

bool ContactManifold::hasPenetratingPoints(f32 epsilon) const {
    for (u32 i = 0u; i < pointCount; ++i) {
        if (points[i].penetration > epsilon) {
            return true;
        }
    }
    return false;
}

void ContactManifold::invalidateFrictionBasis() {
    frictionBasis = {};
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
