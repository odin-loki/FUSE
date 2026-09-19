#include <fuse/physics/narrowphase/contact_manifold.hpp>
#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/contact_pair.hpp>
#include <fuse/physics/narrowphase/friction.hpp>

#include <fuse/physics/narrowphase/contact_pair.hpp>

#include <fuse/physics/narrowphase/friction.hpp>

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

bool ContactManifold::needsNormalNormalization(f32 lengthEpsilon) const {
    return hasNonUnitNormal(lengthEpsilon);
bool ContactManifold::hasUnitNormal(f32 epsilon) const {
    return std::fabs(contactNormal.length() - 1.f) <= epsilon;
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
    if (should_skip_manifold_prune(*this, separationEpsilon, duplicateEpsilon)) {
        return !empty();
    }
    return pruneIfEmpty(separationEpsilon, duplicateEpsilon);

bool ContactManifold::canSkipPruneShallowPenetrations(f32 minDepth) const {
    return !hasShallowPenetrations(minDepth);

bool ContactManifold::pruneShallowPenetrationsIfNeeded(f32 minDepth) {
    if (canSkipPruneShallowPenetrations(minDepth)) {
    pruneShallowPenetrations(minDepth);

bool ContactManifold::normalizeContactNormalIfNeeded(f32 lengthEpsilon) {
    if (!hasValidNormal()) {
        return false;
    }
    if (!needsNormalNormalization(lengthEpsilon)) {
        return true;
    }

    const f32 normalLength = contactNormal.length();
    contactNormal = contactNormal * (1.f / normalLength);
    return hasValidNormal();
}

bool ContactManifold::normalizeContactNormal(f32 lengthEpsilon) {
    if (!hasValidNormal()) {
        return false;
    }
    const f32 normalLength = contactNormal.length();
    if (normalLength <= lengthEpsilon) {
        return false;
    }
    contactNormal = contactNormal * (1.f / normalLength);
    return true;
}

bool ContactManifold::normalizeContactNormalIfNeeded(f32 lengthEpsilon) {
    if (!needsNormalNormalization(lengthEpsilon)) {
        return hasValidNormal();
    }
    return normalizeContactNormal(lengthEpsilon);
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldPruneRejectReason::AllSeparated:
        return "AllSeparated";
    return "Unknown";

ManifoldPruneRejectReason manifold_prune_reject_reason(
}

        return !empty();
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    case ManifoldPruneRejectReason::NoPruningNeeded:
        return "NoPruningNeeded";
    case ManifoldPruneRejectReason::ExceedsMaxPoints:
        return "ExceedsMaxPoints";
    case ManifoldPruneRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldPruneRejectReason::NonUnitNormal:
        return "NonUnitNormal";
    case ManifoldPruneRejectReason::NeedsNormalNormalize:
        return "NeedsNormalNormalize";
    }
    return "Unknown";
    case ManifoldPruneRejectReason::AllSeparated:
        return "AllSeparated";
    case ManifoldPruneRejectReason::Empty:
        return "Empty";
    case ManifoldPruneRejectReason::AlreadyClean:
        return "AlreadyClean";
    case ManifoldPruneRejectReason::NothingToPrune:
        return "NothingToPrune";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldPruneRejectReason::InvalidNormal;
        return ManifoldPruneRejectReason::NonUnitNormal;
    if (manifold.pointCount > kMaxContactPointsPerManifold) {
        return ManifoldPruneRejectReason::ExceedsMaxPoints;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    if (!manifold.needsPruning(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::NoPruningNeeded;
    return ManifoldPruneRejectReason::None;

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

bool can_skip_manifold_prune(
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) !=
           ManifoldPruneRejectReason::None;

bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,
    if (can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon)) {
        return !manifold.empty();
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);
    }
        return ManifoldPruneRejectReason::AllSeparated;

    const ContactManifold& manifold,
    f32 separationEpsilon,



    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
        return ManifoldPruneRejectReason::Empty;
        return ManifoldPruneRejectReason::WouldBeEmpty;

    const bool hasSeparated = manifold.hasSeparatedPoints(separationEpsilon);
    const bool hasDuplicates = manifold.hasDuplicatePoints(duplicateEpsilon);
    const bool exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    const bool hasShallow = shallowMinDepth > 0.f && manifold.hasShallowPenetrations(shallowMinDepth);

    if (!hasSeparated && !hasDuplicates && !exceedsMaxPoints && !hasShallow) {
        return ManifoldPruneRejectReason::AlreadyClean;


    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) == expected;
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth).reason;

    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           expected;
    const bool needsPruning = hasSeparated || hasDuplicates || exceedsMaxPoints;
    const bool needsShallowPruning =
        shallowMinDepth > 0.f && manifold.hasShallowPenetrations(shallowMinDepth);
    if (!needsPruning && !needsShallowPruning) {
        return ManifoldPruneRejectReason::NothingToPrune;

    const bool needsPruning = manifold.needsPruning(separationEpsilon, duplicateEpsilon);
    const bool needsShallow = shallowMinDepth > 0.f && manifold.hasShallowPenetrations(shallowMinDepth);
    if (!needsPruning && !needsShallow) {



bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) == expected;
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::CleanManifold:
        return "CleanManifold";
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (prunePreflight.can_skip_prune(shallowMinDepth)) {
        return ManifoldPruneRejectReason::CleanManifold;
    }
    if (prunePreflight.wouldBeEmpty) {
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    }
    if (manifold.pointCount > kMaxContactPointsPerManifold) {
        return ManifoldPruneRejectReason::ExceedsMaxPoints;
    }
    if (manifold.pointCount > kMaxContactPointsPerManifold) {
        return ManifoldPruneRejectReason::ExceedsMaxPoints;
    }
    return ManifoldPruneRejectReason::None;
}

bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) == expected;
    f32 duplicateEpsilon) {
    if (expected == ManifoldPruneRejectReason::NeedsNormalNormalize) {
        return !manifold.empty() && manifold.needsNormalNormalization();
    }
    if (expected == ManifoldPruneRejectReason::ExceedsMaxPoints) {
        return manifold.pointCount > kMaxContactPointsPerManifold;
    }
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           ManifoldPruneRejectReason::None;
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool normalize_contact_normal_if_needed(ContactManifold& manifold, f32 lengthEpsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    }
    if (!manifold.needsNormalNormalization(lengthEpsilon)) {
        return false;
    }

    const f32 normalLength = manifold.contactNormal.length();
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    return true;
}

bool is_contact_manifold_finalized(
    const ContactManifold& manifold,
    f32 lengthEpsilon,
    f32 frictionEpsilon) {
    if (!manifold.valid || manifold.empty()) {
        return false;
    }
    if (!manifold.hasValidNormal() || manifold.hasNonUnitNormal(lengthEpsilon)) {
        return false;
    }
    if (!manifold.hasPenetratingPoints()) {
        return false;
    }
    return can_skip_friction_basis_rebuild(manifold, frictionEpsilon);
}

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }

    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    if (prunePreflight.wouldBeEmpty) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    }
    return ManifoldFinalizeRejectReason::None;
}

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon).can_finalize();
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::NoPruningNeeded:
        return "NoPruningNeeded";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }

    const bool needsPruning = manifold.needsPruning(separationEpsilon, duplicateEpsilon);
    const bool needsShallow = shallowMinDepth > 0.f && manifold.hasShallowPenetrations(shallowMinDepth);
    if (!needsPruning && !needsShallow) {
        return ManifoldPruneRejectReason::NoPruningNeeded;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::WouldBeEmpty;
    }
    return ManifoldPruneRejectReason::None;
}

bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           expected;
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

ManifoldPrunePreflight preflight_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return preflight_manifold_prune_ex(manifold, separationEpsilon, duplicateEpsilon, 0.f);
}

ManifoldPrunePreflight preflight_manifold_prune_ex(
    if (minDepth <= 0.f) {
        return true;

ManifoldPrunePreflight preflight_manifold_prune(
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPrunePreflight preflight{};
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.reason =
        manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    preflight.reason = manifold_prune_reject_reason(
        manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::AllSeparated;
    return ManifoldPruneRejectReason::None;
    if (preflight.reason == ManifoldPruneRejectReason::EmptyManifold) {
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.reason == ManifoldPruneRejectReason::Empty) {
        preflight.reason = ManifoldPruneRejectReason::EmptyManifold;
        preflight.skipped = true;
        preflight.wouldBeEmpty = true;
        preflight.reason = ManifoldPruneRejectReason::EmptyManifold;
        return preflight;
    }

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

ManifoldPrunePreflight preflight_manifold_prune(
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPrunePreflight preflight{};
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.reason != ManifoldPruneRejectReason::None) {
    if (preflight.reason == ManifoldPruneRejectReason::EmptyManifold) {
        preflight.skipped = true;
        preflight.wouldBeEmpty = preflight.reason == ManifoldPruneRejectReason::AllSeparated ||
                                 preflight.reason == ManifoldPruneRejectReason::EmptyManifold;
        preflight.wouldBeEmpty = true;
        preflight.reason = ManifoldPruneRejectReason::EmptyManifold;
        return preflight;

    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.hasSeparated = manifold.hasSeparatedPoints(separationEpsilon);
    preflight.hasDuplicates = manifold.hasDuplicatePoints(duplicateEpsilon);
    if (shallowMinDepth > 0.f) {
        preflight.hasShallow = manifold.hasShallowPenetrations(shallowMinDepth);
        preflight.hasShallowPenetrations = manifold.hasShallowPenetrations(shallowMinDepth);
    }
    preflight.exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    if (shallowMinDepth > 0.f) {
        preflight.hasShallow = manifold.hasShallowPenetrations(shallowMinDepth);
    }
    preflight.wouldBeEmpty = manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    preflight.needsNormalNormalize = manifold.needsNormalNormalization();
    if (!preflight.needs_pruning() && !preflight.needs_shallow_pruning(shallowMinDepth)) {
        preflight.reason = ManifoldPruneRejectReason::AlreadyClean;
    }
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    return preflight;

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
    preflight.wouldBeEmpty = preflight.reason == ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    return preflight;

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    }
    return "Unknown";

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    case ManifoldPruneRejectReason::None:
    case ManifoldPruneRejectReason::EmptyManifold:
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:

ManifoldPruneRejectReason manifold_prune_reject_reason(

    case ManifoldFinalizeRejectReason::Empty:
        return "Empty";
    case ManifoldFinalizeRejectReason::EmptyAfterPrune:
        return "EmptyAfterPrune";

    case ManifoldFinalizeRejectReason::NoPenetration:
        return "NoPenetration";

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);


    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    return ManifoldPruneRejectReason::None;

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

bool should_skip_manifold_prune(
    return !manifold.needsPruning(separationEpsilon, duplicateEpsilon);

bool can_skip_manifold_prune(
        return true;
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);


        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;


        return ManifoldFinalizeRejectReason::Empty;
        return ManifoldFinalizeRejectReason::EmptyAfterPrune;

        return ManifoldFinalizeRejectReason::NoPenetration;


    f32 frictionEpsilon) {
    (void)frictionEpsilon;

    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) ==
           expected;

bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,
    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        return !manifold.empty();

    if (shallowMinDepth > 0.f && manifold.hasShallowPenetrations(shallowMinDepth)) {
        manifold.pruneShallowPenetrations(shallowMinDepth);
            return false;

    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);


    f32 /*frictionEpsilon*/) {

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    if (prunePreflight.wouldBeEmpty) {




    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) == expected;



    case ManifoldFinalizeRejectReason::MissingFrictionBasis:
        return "MissingFrictionBasis";
    case ManifoldFinalizeRejectReason::NeedsNormalNormalize:
        return "NeedsNormalNormalize";
    case ManifoldFinalizeRejectReason::AlreadyFinalized:
        return "AlreadyFinalized";
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    }
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }
    return ManifoldFinalizeRejectReason::None;
}

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (expected == ManifoldFinalizeRejectReason::NeedsNormalNormalize) {
        return manifold.needsNormalNormalization();
    }
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

bool can_skip_manifold_prune_dispatch(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           ManifoldPruneRejectReason::None;
}

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    }
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }
    return ManifoldFinalizeRejectReason::None;
}

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (expected == ManifoldFinalizeRejectReason::AlreadyFinalized) {
        return is_contact_manifold_finalized(manifold, 1e-4f, 1e-4f);
    }
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    }
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }
    return ManifoldFinalizeRejectReason::None;
}

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) ==
           expected;
}

bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::AllClean:
        return "AllClean";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.wouldBeEmpty) {
        return ManifoldPruneRejectReason::WouldBeEmpty;
    }
    if (preflight.can_skip_prune(shallowMinDepth)) {
        return ManifoldPruneRejectReason::AllClean;
    }
    return ManifoldPruneRejectReason::None;
}

bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) == expected;
}

bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::AlreadyClean:
        return "AlreadyClean";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::WouldBeEmpty;
    }
    if (manifold.canSkipPruneContactPoints(separationEpsilon, duplicateEpsilon) &&
        (shallowMinDepth <= 0.f || manifold.canSkipPruneShallowPenetrations(shallowMinDepth))) {
        return ManifoldPruneRejectReason::AlreadyClean;
    }
    return ManifoldPruneRejectReason::None;
}

bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           expected;
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    switch (reason) {
    case ManifoldFinalizeRejectReason::None:
        return "None";
    case ManifoldFinalizeRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    }
    return "Unknown";
}

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 /*frictionEpsilon*/) {
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    }
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    }
    return ManifoldFinalizeRejectReason::None;
}

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) ==
           expected;
}

ManifoldFinalizePreflight preflight_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    ManifoldFinalizePreflight preflight{};
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.rejectReason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    preflight.reason =
        manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    preflight.reason = manifold_finalize_reject_reason(
        manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (manifold.empty()) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    }
    if (!manifold.hasValidNormal()) {
        return ManifoldFinalizeRejectReason::InvalidNormal;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    (void)frictionEpsilon;
    return ManifoldFinalizeRejectReason::None;

bool manifold_finalize_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldFinalizeRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) ==
           expected;

ManifoldFinalizePreflight preflight_manifold_finalize(
    ManifoldFinalizePreflight preflight{};
    if (preflight.reason != ManifoldFinalizeRejectReason::None) {
        preflight.wouldBeEmptyAfterPrune =
            preflight.reason == ManifoldFinalizeRejectReason::AllSeparatedAfterPrune ||
            preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold;
    preflight.reason = manifold_finalize_reject_reason(
        manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold) {
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (preflight.reason == ManifoldFinalizeRejectReason::Empty) {
        preflight.skipped = true;
        preflight.skipped = preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold;
        if (preflight.reason == ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune) {
            preflight.wouldBeEmptyAfterPrune = true;
        return preflight;
    }

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.needsPruning = prunePreflight.needs_pruning();
    preflight.wouldBeEmptyAfterPrune = prunePreflight.wouldBeEmpty;

    if (preflight.reason != ManifoldFinalizeRejectReason::None) {
        return preflight;
    }

    preflight.needsNormalNormalize = manifold.needsNormalNormalization(frictionEpsilon);
    if (preflight.reason != ManifoldFinalizeRejectReason::None) {
        return preflight;
    }

    preflight.canFinalize = true;
    preflight.needsNormalNormalization = !manifold.hasUnitNormal(frictionEpsilon);
    preflight.rejectReason = ManifoldFinalizeRejectReason::None;
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
    if (shallowMinDepth > 0.f) {
        preflight.hasShallow = manifold.hasShallowPenetrations(shallowMinDepth);
    return preflight;
}

bool can_skip_manifold_prune(const ManifoldPrunePreflight& preflight) {
    return preflight.can_skip();
}

bool ContactManifold::canSkipPruneShallowPenetrations(f32 minDepth) const {
    return !hasShallowPenetrations(minDepth);

bool ContactManifold::pruneShallowPenetrationsIfNeeded(f32 minDepth) {
    if (canSkipPruneShallowPenetrations(minDepth)) {
        return !empty();
    pruneShallowPenetrations(minDepth);

bool ContactManifold::pruneFromPreflight(
    const ManifoldPrunePreflight& preflight,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (preflight.skipped || preflight.wouldBeEmpty) {
        if (preflight.wouldBeEmpty) {
            clear();
        return false;

    if (!preflight.needs_pruning()) {

    return pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);
bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    return manifold.canSkipPruneContactPoints(separationEpsilon, duplicateEpsilon);
ManifoldFinalizePreflight preflight_manifold_finalize(
    ManifoldFinalizePreflight preflight{};
    if (manifold.empty()) {
        preflight.empty = true;
        preflight.skipped = true;
        return preflight;

    preflight.invalidNormal = !manifold.hasValidNormal();
    preflight.noPenetratingPoints = !manifold.hasPenetratingPoints();

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.needsPrune = prunePreflight.needs_pruning();
    preflight.wouldBeEmptyAfterPrune = prunePreflight.wouldBeEmpty;

bool should_skip_manifold_finalize(
    return !preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon).can_finalize();

bool generate_contact_manifold_if_needed(ContactManifold& manifold) {
    if (should_skip_manifold_finalize(manifold)) {
        manifold.clear();
    return generate_contact_manifold(manifold);
bool should_skip_manifold_prune(
    const ManifoldPrunePreflight preflight =
    return preflight.skipped || !preflight.needs_pruning();

bool prune_contact_points_guarded(
    ContactManifold& manifold,
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);

ManifoldFinalizePreflight preflight_finalize_contact_manifold(
        preflight.wouldFail = true;
        preflight.prune.skipped = true;
        preflight.prune.wouldBeEmpty = true;

    preflight.prune = preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.canFinalize = can_finalize_contact_manifold(manifold);
    preflight.wouldFail = preflight.prune.wouldBeEmpty || !preflight.canFinalize;

bool should_skip_finalize_contact_manifold(const ContactManifold& manifold) {
    return !can_finalize_contact_manifold(manifold);

bool prune_shallow_penetrations_if_needed(ContactManifold& manifold, f32 minDepth) {
    if (!manifold.hasShallowPenetrations(minDepth)) {
        return !manifold.empty();
    manifold.pruneShallowPenetrations(minDepth);

bool generate_contact_manifold_if_ready(ContactManifold& manifold) {
    const ManifoldFinalizePreflight preflight = preflight_finalize_contact_manifold(manifold);
    if (!preflight.can_finalize()) {
ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold) {
        preflight.isEmpty = true;

    preflight.hasInvalidNormal = !manifold.hasValidNormal();
    preflight.hasNoPenetratingPoints = !manifold.hasPenetratingPoints();

bool should_skip_manifold_finalize(const ContactManifold& manifold) {
    return !preflight_manifold_finalize(manifold).can_finalize();

bool can_finalize_with_preflight(const ManifoldFinalizePreflight& preflight) {
    return preflight.can_finalize();

    preflight.empty = false;
    preflight.noPenetrating = !manifold.hasPenetratingPoints();
    preflight.needsPruning = manifold.needsPruning();
    preflight.needsNormalization = needs_normal_normalization(manifold);

bool needs_normal_normalization(const ContactManifold& manifold, f32 epsilon) {
    if (!manifold.hasValidNormal()) {
    return std::fabs(manifold.contactNormal.length() - 1.f) > epsilon;

bool can_skip_manifold_finalize(const ContactManifold& manifold) {

bool generate_contact_manifold_guarded(ContactManifold& manifold) {
    const ManifoldFinalizePreflight preflight = preflight_manifold_finalize(manifold);

    preflight.hasValidNormal = manifold.hasValidNormal();
    preflight.hasPenetrating = manifold.hasPenetratingPoints(separationEpsilon);
    preflight.needsFrictionBasis = needs_friction_basis_refresh(manifold);

ManifoldPruneDispatchPreflight preflight_manifold_prune_dispatch(
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPruneDispatchPreflight preflight{};
        preflight.wouldBeEmpty = true;

    const ManifoldPrunePreflight regularPreflight =
    preflight.needsRegularPrune = regularPreflight.needs_pruning();
    preflight.wouldBeEmpty = regularPreflight.wouldBeEmpty;
    if (shallowMinDepth > 0.f) {
        preflight.needsShallowPrune = manifold.hasShallowPenetrations(shallowMinDepth);

bool can_skip_manifold_prune_dispatch(
    return preflight_manifold_prune_dispatch(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_skip();

bool prune_contact_manifold_if_needed(
    const ManifoldPruneDispatchPreflight preflight =
        preflight_manifold_prune_dispatch(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.can_skip()) {

    if (preflight.needsRegularPrune) {
        if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {

    if (shallowMinDepth > 0.f && preflight.needsShallowPrune) {
        return manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);


bool can_skip_manifold_finalize(

bool generate_contact_manifold_if_valid(ContactManifold& manifold) {
const char* manifold_finalize_failure_reason_name(ManifoldFinalizeFailureReason reason) {
    switch (reason) {
    case ManifoldFinalizeFailureReason::None:
        return "None";
    case ManifoldFinalizeFailureReason::Empty:
        return "Empty";
    case ManifoldFinalizeFailureReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeFailureReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeFailureReason::PruneWouldEmpty:
        return "PruneWouldEmpty";
    case ManifoldFinalizeFailureReason::MissingFrictionBasis:
        return "MissingFrictionBasis";
    return "Unknown";

        preflight.reason = ManifoldFinalizeFailureReason::Empty;

    if (preflight.prune.wouldBeEmpty) {
        preflight.reason = ManifoldFinalizeFailureReason::PruneWouldEmpty;

        preflight.reason = ManifoldFinalizeFailureReason::InvalidNormal;

    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        preflight.reason = ManifoldFinalizeFailureReason::NoPenetratingPoints;



ManifoldFinalizeResult generate_contact_manifold_result(ContactManifold& manifold) {
    ManifoldFinalizeResult result{};
    result.reason = preflight.reason;
        result.skipped = true;
        if (preflight.reason == ManifoldFinalizeFailureReason::Empty ||
            preflight.reason == ManifoldFinalizeFailureReason::PruneWouldEmpty ||
            preflight.reason == ManifoldFinalizeFailureReason::NoPenetratingPoints) {
        } else if (preflight.reason == ManifoldFinalizeFailureReason::InvalidNormal ||
                   preflight.reason == ManifoldFinalizeFailureReason::MissingFrictionBasis) {
        return result;

    if (!manifold.pruneContactPointsIfNeeded()) {
        result.reason = ManifoldFinalizeFailureReason::PruneWouldEmpty;

        result.reason = ManifoldFinalizeFailureReason::InvalidNormal;

    const f32 normalLength = manifold.contactNormal.length();
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

    manifold.syncLegacyFields();
    compute_friction_tangents_if_needed(manifold);
    if (!manifold.hasFrictionBasis()) {
        result.reason = ManifoldFinalizeFailureReason::MissingFrictionBasis;

    manifold.valid = true;
    result.finalized = true;

ManifoldFinalizeResult generate_contact_manifold_guarded(ContactManifold& manifold) {
    return generate_contact_manifold_result(manifold);

    return generate_contact_manifold_guarded(manifold).finalized;
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) !=
           ManifoldFinalizeRejectReason::None;
}

bool should_skip_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    return preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)
        .can_skip_finalize();
}

bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon).can_skip_prune();

bool prune_manifold_if_needed(
bool should_run_manifold_finalize(
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
}

bool prune_contact_manifold_with_preflight(
    ContactManifold& manifold,
    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon)) {
        return !manifold.empty();
    return manifold.pruneIfEmpty(separationEpsilon, duplicateEpsilon);

bool finalize_manifold_if_needed(
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (preflight.can_skip_finalize()) {
        return false;
    return generate_contact_manifold(manifold);

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::Empty:
        return "Empty";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
    return "Unknown";

ManifoldPruneRejectReason manifold_prune_reject_reason(
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::Empty;

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.wouldBeEmpty) {
        return ManifoldPruneRejectReason::WouldBeEmpty;

    return ManifoldPruneRejectReason::None;

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;


bool should_run_manifold_prune(
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);

bool prune_contact_manifold_if_needed(
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);

bool should_run_manifold_finalize(
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

bool can_skip_manifold_prune(
    f32 shallowMinDepth) {
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    return preflight.skipped || !preflight.needs_any_pruning(shallowMinDepth);

    if (can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {

    if (preflight.needs_pruning()) {
        if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        if (!manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth)) {

bool finalize_contact_manifold_if_needed(
    if (!preflight.can_finalize()) {

bool ContactManifold::hasUnnormalizedNormal(f32 epsilon) const {
    if (!hasValidNormal()) {
    const f32 normalLength = contactNormal.length();
    return std::fabs(normalLength - 1.f) > epsilon;

bool ContactManifold::canSkipNormalizeContactNormal(f32 epsilon) const {
    return !hasUnnormalizedNormal(epsilon);

void ContactManifold::normalizeContactNormalIfNeeded(f32 epsilon) {
    if (!hasUnnormalizedNormal(epsilon)) {
        return;
    if (normalLength < 1e-8f) {
    contactNormal = contactNormal * (1.f / normalLength);

ManifoldFinalizeDeepenPreflight preflight_manifold_finalize_deepen(
    f32 frictionEpsilon,
    f32 normalEpsilon) {
    ManifoldFinalizeDeepenPreflight preflight{};
        preflight.skipped = true;
        return preflight;

    const ManifoldFinalizePreflight basePreflight =
    preflight.canFinalize = basePreflight.canFinalize;
    preflight.needsPruning = basePreflight.needsPruning;
    preflight.wouldBeEmptyAfterPrune = basePreflight.wouldBeEmptyAfterPrune;
    preflight.needsFrictionBasis = basePreflight.needsFrictionBasis;
    preflight.canReuseFrictionBasis = basePreflight.canReuseFrictionBasis;
    preflight.needsNormalNormalization = manifold.hasUnnormalizedNormal(normalEpsilon);

bool can_skip_manifold_finalize_deepen(
    return !preflight_manifold_finalize_deepen(
                manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon, normalEpsilon)
                .can_finalize();
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) !=
           ManifoldFinalizeRejectReason::None;
}

bool generate_contact_manifold_deepen_if_needed(ContactManifold& manifold) {
    if (can_skip_manifold_finalize_deepen(manifold)) {
        return false;
    }
    return generate_contact_manifold_if_needed(manifold);
const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    return "Unknown";

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    return ManifoldPruneRejectReason::None;

bool manifold_prune_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
ManifoldGeneratePreflight preflight_generate_contact_manifold(
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    ManifoldGeneratePreflight preflight{};
        preflight.skipped = true;
        return preflight;

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.needsPruning = prunePreflight.needs_pruning();
    preflight.wouldBeEmptyAfterPrune = prunePreflight.wouldBeEmpty;

    if (!manifold.hasValidNormal()) {

    if (preflight.wouldBeEmptyAfterPrune) {

    if (!manifold.hasPenetratingPoints(separationEpsilon)) {

    preflight.canGenerate = true;
    preflight.needsFrictionBasis = needs_friction_basis_refresh(manifold, frictionEpsilon);

bool can_skip_generate_contact_manifold(
    return !preflight_generate_contact_manifold(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)
                .can_generate();

ManifoldPruneChainPreflight preflight_manifold_prune_chain(
    f32 shallowMinDepth) {
    ManifoldPruneChainPreflight preflight{};
        preflight.wouldBeEmpty = true;

    preflight.needsSeparationPrune = manifold.hasSeparatedPoints(separationEpsilon);
    preflight.needsDuplicatePrune = manifold.hasDuplicatePoints(duplicateEpsilon);
    preflight.needsMaxPointPrune = manifold.pointCount > kMaxContactPointsPerManifold;
    if (shallowMinDepth > 0.f) {
        preflight.needsShallowPrune = manifold.hasShallowPenetrations(shallowMinDepth);
    preflight.wouldBeEmpty = manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);

bool can_skip_manifold_prune(
    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.skipped) {
    return !preflight.needs_pruning();
bool finalize_contact_manifold_if_needed(
    ContactManifold& manifold,
    return preflight.skipped || !preflight.needs_any_pruning(shallowMinDepth);

bool prune_manifold_if_needed(
    if (can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        return !manifold.empty();

    if (preflight.needs_pruning()) {
        if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        if (!manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth)) {

    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (!preflight.can_finalize()) {

    if (preflight.needsPruning) {
            manifold.clear();


    if (preflight.needsNormalNormalization) {
        const f32 normalLength = manifold.contactNormal.length();
        manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);

    manifold.syncLegacyFields();
    compute_friction_tangents_if_needed(manifold, frictionEpsilon);
    if (!manifold.hasFrictionBasis()) {

    manifold.valid = true;
    return true;

bool generate_contact_manifold_deepen(
    f32 shallowMinDepth,

        manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);

    return finalize_contact_manifold_if_needed(
        manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (preflight.skipped || preflight.reason != ManifoldPruneRejectReason::None) {
        if (preflight.reason == ManifoldPruneRejectReason::AllSeparated) {
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);

bool finalize_contact_manifold_if_needed(ContactManifold& manifold) {
    if (!preflight_manifold_finalize(manifold).can_finalize()) {
    return generate_contact_manifold(manifold);
    return manifold.canSkipPruneContactPoints(separationEpsilon, duplicateEpsilon);

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    case ManifoldFinalizeRejectReason::None:
    case ManifoldFinalizeRejectReason::EmptyManifold:
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
        return ManifoldFinalizeRejectReason::EmptyManifold;
        return ManifoldFinalizeRejectReason::InvalidNormal;
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    return ManifoldFinalizeRejectReason::None;

bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
    return !preflight_manifold_prune_chain(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
                .needs_pruning();

bool prune_contact_points_if_needed(

ManifoldFinalizeChainPreflight preflight_manifold_finalize_chain(
    ManifoldFinalizeChainPreflight preflight{};

    preflight.prune = preflight_manifold_prune_chain(
        manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    preflight.generate =
        preflight_generate_contact_manifold(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

    if (can_skip_generate_contact_manifold(manifold)) {
bool should_skip_manifold_prune(
    return preflight.skipped || !preflight.needs_pruning();

bool can_prune_manifold_in_place(
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon).can_prune_in_place();
}

bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
}

bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    return preflight.skipped || !preflight.needs_any_pruning(shallowMinDepth);
}

bool prune_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        return !manifold.empty();
    }

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.needs_pruning()) {
        if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {
            return false;
        }
    }
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        if (!manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth)) {
            return false;
        }
    }
    return !manifold.empty();
}

bool finalize_contact_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (!preflight.can_finalize()) {
        return false;
    }
    return generate_contact_manifold(manifold);
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::Empty:
        return "Empty";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::Empty;
    }

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.wouldBeEmpty) {
        return ManifoldPruneRejectReason::WouldBeEmpty;
    }

    return ManifoldPruneRejectReason::None;
}

bool manifold_prune_rejects_for_reason(
    const ContactManifold& manifold,
    ManifoldPruneRejectReason expected,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool can_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_skip_prune(shallowMinDepth);
}

bool prune_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        return !manifold.empty();
    }

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.needs_pruning()) {
        if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {
            return false;
        }
    }
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        if (!manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth)) {
            return false;
        }
    return !manifold.empty();

bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);

bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

bool finalize_contact_manifold_if_needed(
    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (!preflight.can_finalize()) {
    return generate_contact_manifold_with_preflight(manifold);

bool can_skip_generate_contact_manifold(const ContactManifold& manifold) {
    return !can_finalize_contact_manifold(manifold);

bool generate_contact_manifold_with_preflight(ContactManifold& manifold) {
    if (!can_finalize_contact_manifold(manifold)) {
        return false;
    }
    return generate_contact_manifold(manifold);
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:
        return "WouldBeEmptyAfterPrune";
    case ManifoldPruneRejectReason::NoPruneNeeded:
        return "NoPruneNeeded";
    }
    return "Unknown";

ManifoldPruneRejectReason manifold_prune_reject_reason(
bool manifold_prune_preflight_skips(
bool normalize_contact_normal_if_needed(ContactManifold& manifold, f32 lengthEpsilon) {
    if (!manifold.hasValidNormal()) {
        return false;
    if (!manifold.needsNormalNormalization(lengthEpsilon)) {
        return true;

    const f32 normalLength = manifold.contactNormal.length();
    manifold.contactNormal = manifold.contactNormal * (1.f / normalLength);
    return manifold.hasValidNormal();

ManifoldBeyondPrunePreflight preflight_manifold_beyond_prune(

bool should_run_manifold_prune(


ManifoldPruneRejectReason manifold_prune_second_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    const ManifoldPruneRejectReason baseReason =
        manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (baseReason != ManifoldPruneRejectReason::None) {
        return baseReason;
    if (manifold.pointCount >= kMaxContactPointsPerManifold &&
        manifold.needsPruning(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::ExceedsMaxPoints;
    return ManifoldPruneRejectReason::None;

bool manifold_prune_second_rejects_for_reason(
    ManifoldPruneRejectReason expected,
    return manifold_prune_second_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;

bool can_skip_manifold_prune_second(
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;

    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.wouldBeEmpty) {
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    if (preflight.can_skip_prune(shallowMinDepth)) {
        return ManifoldPruneRejectReason::NoPruneNeeded;

bool manifold_prune_rejects_for_reason(
    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) == expected;

    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_skip_prune(shallowMinDepth);

bool prune_contact_manifold_if_needed(
    ContactManifold& manifold,
    const ManifoldPruneRejectReason reason =
        manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (reason == ManifoldPruneRejectReason::EmptyManifold ||
        reason == ManifoldPruneRejectReason::WouldBeEmptyAfterPrune) {
        manifold.clear();
    if (reason == ManifoldPruneRejectReason::NoPruneNeeded) {
        return !manifold.empty();

    manifold.pruneContactPoints(separationEpsilon, duplicateEpsilon);
    if (shallowMinDepth > 0.f) {
        manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);

const char* manifold_finalize_reject_reason_name(ManifoldFinalizeRejectReason reason) {
    case ManifoldFinalizeRejectReason::None:
    case ManifoldFinalizeRejectReason::EmptyManifold:
    case ManifoldFinalizeRejectReason::InvalidNormal:
        return "InvalidNormal";
    case ManifoldFinalizeRejectReason::NoPenetratingPoints:
        return "NoPenetratingPoints";
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:

ManifoldFinalizeRejectReason manifold_finalize_reject_reason(
    f32 frictionEpsilon) {
        return ManifoldFinalizeRejectReason::EmptyManifold;
    if (!manifold.hasValidNormal(frictionEpsilon)) {
        return ManifoldFinalizeRejectReason::InvalidNormal;

    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (preflight.wouldBeEmptyAfterPrune) {
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    if (!manifold.hasPenetratingPoints(separationEpsilon)) {
        return ManifoldFinalizeRejectReason::NoPenetratingPoints;
    if (!preflight.can_finalize()) {
    return ManifoldFinalizeRejectReason::None;

bool manifold_finalize_rejects_for_reason(
    ManifoldFinalizeRejectReason expected,
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) == expected;

bool should_run_manifold_finalize(
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (manifold_prune_preflight_skips(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
    return prune_contact_manifold_with_preflight(
        manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

bool finalize_contact_manifold_if_needed(
    if (!should_run_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)) {
    return generate_contact_manifold(manifold);

    if (!can_finalize_contact_manifold(manifold)) {
    return finalize_contact_manifold_with_preflight(
        manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

bool manifold_finalize_preflight_skips(



    return preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon).can_finalize();

ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    f32 shallowMinDepth,
    ManifoldPruneFinalizePreflight preflight{};
    preflight.prune = preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    preflight.finalize =
    return preflight;

bool should_skip_manifold_prune_finalize(
    return preflight_manifold_prune_finalize(
               manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth, frictionEpsilon)
        .can_skip_all(shallowMinDepth);

    case ManifoldPruneRejectReason::NoPruningNeeded:
        return "NoPruningNeeded";


        return ManifoldPruneRejectReason::NoPruningNeeded;

    return manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth) ==
           expected;


bool can_skip_manifold_prune(
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);



    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    if (prunePreflight.wouldBeEmpty) {

    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;


    case ManifoldPruneRejectReason::AllValid:
        return "AllValid";
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return "WouldBeEmpty";


        return ManifoldPruneRejectReason::AllValid;
        return ManifoldPruneRejectReason::WouldBeEmpty;



    case ManifoldFinalizeRejectReason::NoPenetration:
        return "NoPenetration";


        return ManifoldFinalizeRejectReason::NoPenetration;
    if (preflight.can_finalize()) {









    case ManifoldPruneRejectReason::AlreadyClean:
        return "AlreadyClean";

    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
        return ManifoldPruneRejectReason::AlreadyClean;




    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {

    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) ==




bool prune_manifold_if_needed(

    if (!manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon)) {

        return manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);



bool prune_manifold_using_preflight(
    if (preflight.skipped || preflight.can_skip_prune(shallowMinDepth)) {


    if (preflight.needs_pruning()) {

    if (shallowMinDepth > 0.f && preflight.hasShallow) {
        if (!manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth)) {


bool finalize_manifold_using_preflight(

ManifoldProcessPreflight preflight_manifold_process(
    ManifoldProcessPreflight process{};
    process.prune = preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    process.finalize =
    return process;

void normalize_contact_normal_if_needed(ContactManifold& manifold, f32 lengthEpsilon) {
        return;
    if (normalLength > 1e-8f) {





bool finalize_contact_manifold_with_preflight(
    if (can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)) {

    if (!prune_contact_manifold_if_needed(manifold, separationEpsilon, duplicateEpsilon)) {

    normalize_contact_normal_if_needed(manifold, frictionEpsilon);
    compute_friction_tangents_with_preflight(manifold, frictionEpsilon);

    if (!manifold.hasFrictionBasis()) {

    manifold.syncLegacyFields();
    manifold.valid = true;
    return can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);

    return !can_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

const char* manifold_shallow_prune_reject_reason_name(ManifoldShallowPruneRejectReason reason) {
    case ManifoldShallowPruneRejectReason::None:
    case ManifoldShallowPruneRejectReason::EmptyManifold:
    case ManifoldShallowPruneRejectReason::NoShallowPoints:
        return "NoShallowPoints";

ManifoldShallowPruneRejectReason manifold_shallow_prune_reject_reason(
    f32 minDepth) {
        return ManifoldShallowPruneRejectReason::EmptyManifold;
    if (!manifold.hasShallowPenetrations(minDepth)) {
        return ManifoldShallowPruneRejectReason::NoShallowPoints;
    return ManifoldShallowPruneRejectReason::None;

bool manifold_shallow_prune_rejects_for_reason(
    ManifoldShallowPruneRejectReason expected,
    return manifold_shallow_prune_reject_reason(manifold, minDepth) == expected;

bool prune_shallow_penetrations_with_preflight(ContactManifold& manifold, f32 minDepth) {
    if (manifold_shallow_prune_reject_reason(manifold, minDepth) != ManifoldShallowPruneRejectReason::None) {
    return manifold.pruneShallowPenetrationsIfNeeded(minDepth);
bool prune_and_finalize_contact_manifold_with_preflight(
    f32 frictionEpsilon,
    if (!prune_contact_manifold_with_preflight(
            manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {

bool generate_contact_manifold_with_preflight(
const char* contact_manifold_write_reject_reason_name(ContactManifoldWriteRejectReason reason) {
    case ContactManifoldWriteRejectReason::None:
    case ContactManifoldWriteRejectReason::EmptyManifold:
    case ContactManifoldWriteRejectReason::InvalidNormal:
    case ContactManifoldWriteRejectReason::NotFinalized:
        return "NotFinalized";
    case ContactManifoldWriteRejectReason::SelfPair:
        return "SelfPair";

ContactManifoldWriteRejectReason contact_manifold_write_reject_reason(const ContactManifold& manifold) {
        return ContactManifoldWriteRejectReason::EmptyManifold;
        return ContactManifoldWriteRejectReason::InvalidNormal;
    if (!manifold.valid) {
        return ContactManifoldWriteRejectReason::NotFinalized;
    if (manifold.bodyA == manifold.bodyB) {
        return ContactManifoldWriteRejectReason::SelfPair;
    return ContactManifoldWriteRejectReason::None;

bool contact_manifold_write_rejects_for_reason(
    ContactManifoldWriteRejectReason expected) {
    return contact_manifold_write_reject_reason(manifold) == expected;

ContactManifoldWritePreflight preflight_contact_manifold_buffer_write(const ContactManifold& manifold) {
    ContactManifoldWritePreflight preflight{};
    preflight.reason = contact_manifold_write_reject_reason(manifold);
    preflight.skipped = preflight.reason != ContactManifoldWriteRejectReason::None;

bool should_skip_contact_manifold_buffer_write(const ContactManifold& manifold) {
    return !preflight_contact_manifold_buffer_write(manifold).can_write();
bool finalize_manifold_after_prune_with_preflight(

    case ManifoldShallowPruneRejectReason::NoShallowPenetrations:
        return "NoShallowPenetrations";

        return ManifoldShallowPruneRejectReason::NoShallowPenetrations;


ManifoldShallowPrunePreflight preflight_manifold_shallow_prune(
    ManifoldShallowPrunePreflight preflight{};
    preflight.reason = manifold_shallow_prune_reject_reason(manifold, minDepth);
    if (preflight.reason != ManifoldShallowPruneRejectReason::None) {
        preflight.skipped = true;
    preflight.hasShallow = manifold.hasShallowPenetrations(minDepth);

    if (!preflight_manifold_shallow_prune(manifold, minDepth).can_prune()) {
    if (normalLength <= 1e-8f) {

    if (manifold.empty() || !manifold.hasValidNormal()) {





    if (manifold.valid && manifold.hasFrictionBasis()) {


bool normalize_contact_manifold_normal(ContactManifold& manifold, f32 lengthEpsilon) {
    if (std::fabs(normalLength - 1.f) <= lengthEpsilon) {

bool normalize_contact_manifold_normal_if_needed(ContactManifold& manifold, f32 lengthEpsilon) {
    return normalize_contact_manifold_normal(manifold, lengthEpsilon);

bool can_skip_normalize_contact_manifold_normal(
    f32 lengthEpsilon) {
    return !manifold.needsNormalNormalization(lengthEpsilon);

bool is_finalized_contact_manifold(
    f32 lengthEpsilon,
    if (!manifold.valid || manifold.empty() || !manifold.hasValidNormal()) {
    if (manifold.needsNormalNormalization(lengthEpsilon)) {
    if (!manifold.hasPenetratingPoints()) {
    return can_skip_friction_basis_rebuild(manifold, frictionEpsilon);

    if (can_skip_finalize_contact_manifold_if_finalized(manifold, 1e-4f, frictionEpsilon)) {
        return manifold.valid;

bool can_skip_finalize_contact_manifold_if_finalized(
    return is_finalized_contact_manifold(manifold, lengthEpsilon, frictionEpsilon);

bool manifold_prune_preflight_rejects_for_reason(
               .reason == expected;

bool manifold_finalize_preflight_rejects_for_reason(
    return preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)

    if (preflight.needsPruning) {
                manifold, separationEpsilon, duplicateEpsilon)) {

    ManifoldBeyondPrunePreflight preflight{};
    const ManifoldPrunePreflight basePreflight =
    preflight.reason = basePreflight.reason;
    if (preflight.reason != ManifoldPruneRejectReason::None) {

    preflight.needsNormalNormalize = manifold.needsNormalNormalization();
    preflight.exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    if (preflight.exceedsMaxPoints) {
        preflight.reason = ManifoldPruneRejectReason::ExceedsMaxPoints;

bool should_skip_manifold_beyond_prune(
    const ManifoldBeyondPrunePreflight preflight =
        preflight_manifold_beyond_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.skipped) {
    return preflight.can_skip_prune(shallowMinDepth) &&
           !preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
                .needs_pruning();

bool prune_contact_manifold_beyond_preflight(
    if (preflight.reason == ManifoldPruneRejectReason::AllSeparated) {
    if (preflight.reason == ManifoldPruneRejectReason::EmptyManifold) {

    normalize_contact_normal_if_needed(manifold);
        manifold.pruneToMaxPoints(kMaxContactPointsPerManifold);

bool finalize_contact_manifold_beyond_preflight(

    if (preflight.needsPruning || preflight.needsNormalNormalize) {
        if (!prune_contact_manifold_beyond_preflight(

bool finalize_contact_manifold_if_needed(ContactManifold& manifold) {
    if (!manifold.hasValidNormal() || !manifold.needsNormalNormalization(lengthEpsilon)) {


    if (preflight.skipped || !preflight.can_finalize()) {

bool can_skip_manifold_prune_after_normalize(
    ContactManifold normalized = manifold;
    normalize_contact_normal_if_needed(normalized, lengthEpsilon);
    return should_skip_manifold_prune(
        normalized, separationEpsilon, duplicateEpsilon, shallowMinDepth);


    if (prunePreflight.reason == ManifoldPruneRejectReason::AllSeparated) {
    if (prunePreflight.reason == ManifoldPruneRejectReason::EmptyManifold) {

    if (!prunePreflight.can_skip_prune(shallowMinDepth)) {

    if (!normalize_contact_normal_if_needed(manifold, frictionEpsilon)) {




bool prune_and_finalize_contact_manifold(
    if (!prune_contact_manifold_if_needed(
    return finalize_contact_manifold_if_needed(


        manifold_prune_second_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (reason != ManifoldPruneRejectReason::None) {

bool prune_contact_manifold_second_with_preflight(
        reason == ManifoldPruneRejectReason::AllSeparated) {
        if (reason == ManifoldPruneRejectReason::AllSeparated) {
    if (reason == ManifoldPruneRejectReason::ExceedsMaxPoints) {

ManifoldFinalizeRejectReason manifold_finalize_second_reject_reason(
    const ManifoldFinalizeRejectReason baseReason =
        manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (baseReason != ManifoldFinalizeRejectReason::None &&
        baseReason != ManifoldFinalizeRejectReason::NeedsNormalNormalize) {
    if (manifold.needsNormalNormalization(frictionEpsilon)) {
        return ManifoldFinalizeRejectReason::NeedsNormalNormalize;

bool manifold_finalize_second_rejects_for_reason(
    return manifold_finalize_second_reject_reason(
               manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) == expected;

bool can_skip_manifold_finalize_second(
               manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon) !=
           ManifoldFinalizeRejectReason::None;

bool finalize_contact_manifold_second_with_preflight(
    const ManifoldFinalizeRejectReason reason = manifold_finalize_second_reject_reason(
    if (reason != ManifoldFinalizeRejectReason::None &&
        reason != ManifoldFinalizeRejectReason::NeedsNormalNormalize) {

bool normalize_contact_normal(ContactManifold& manifold, f32 lengthEpsilon) {



    if (can_skip_normalize_contact_normal(manifold, lengthEpsilon)) {
    return normalize_contact_normal(manifold, lengthEpsilon);

bool can_skip_normalize_contact_normal(const ContactManifold& manifold, f32 lengthEpsilon) {




    if (is_contact_manifold_finalized(manifold, frictionEpsilon, frictionEpsilon)) {


    if (normalLength < 1e-8f) {
bool is_finalized_contact_manifold(const ContactManifold& manifold, f32 frictionEpsilon) {
    if (!manifold.valid || manifold.empty()) {
    return manifold.hasFrictionBasis();

bool finalize_contact_manifold_if_not_finalized(
    if (is_finalized_contact_manifold(manifold, frictionEpsilon)) {




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
