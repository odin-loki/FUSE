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
    return !hasShallowPenetrations(minDepth);
}

bool ContactManifold::pruneShallowPenetrationsIfNeeded(f32 minDepth) {
    if (canSkipPruneShallowPenetrations(minDepth)) {
        return !empty();
    }
    pruneShallowPenetrations(minDepth);
    return !empty();
}

const char* manifold_prune_reject_reason_name(ManifoldPruneRejectReason reason) {
    switch (reason) {
    case ManifoldPruneRejectReason::None:
        return "None";
    case ManifoldPruneRejectReason::EmptyManifold:
        return "EmptyManifold";
    case ManifoldPruneRejectReason::AllSeparated:
        return "AllSeparated";
    }
    return "Unknown";
}

ManifoldPruneRejectReason manifold_prune_reject_reason(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon) {
    if (manifold.empty()) {
        return ManifoldPruneRejectReason::EmptyManifold;
    }
    if (manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon)) {
        return ManifoldPruneRejectReason::AllSeparated;
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

ManifoldPrunePreflight preflight_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    ManifoldPrunePreflight preflight{};
    preflight.reason = manifold_prune_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.reason != ManifoldPruneRejectReason::None) {
        preflight.skipped = true;
        preflight.wouldBeEmpty = preflight.reason == ManifoldPruneRejectReason::AllSeparated ||
                                 preflight.reason == ManifoldPruneRejectReason::EmptyManifold;
        return preflight;
    }

    preflight.hasSeparated = manifold.hasSeparatedPoints(separationEpsilon);
    preflight.hasDuplicates = manifold.hasDuplicatePoints(duplicateEpsilon);
    if (shallowMinDepth > 0.f) {
        preflight.hasShallow = manifold.hasShallowPenetrations(shallowMinDepth);
    }
    preflight.exceedsMaxPoints = manifold.pointCount > kMaxContactPointsPerManifold;
    preflight.wouldBeEmpty = manifold.wouldBeEmptyAfterPrune(separationEpsilon, duplicateEpsilon);
    preflight.needsNormalNormalize = manifold.needsNormalNormalization();
    return preflight;
}

bool should_skip_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_skip_prune(shallowMinDepth);
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
    case ManifoldFinalizeRejectReason::AllSeparatedAfterPrune:
        return "AllSeparatedAfterPrune";
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
        return ManifoldFinalizeRejectReason::AllSeparatedAfterPrune;
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
    return manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon) == expected;
}

ManifoldFinalizePreflight preflight_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    ManifoldFinalizePreflight preflight{};
    preflight.reason = manifold_finalize_reject_reason(manifold, separationEpsilon, duplicateEpsilon);
    if (preflight.reason != ManifoldFinalizeRejectReason::None) {
        preflight.skipped = true;
        preflight.wouldBeEmptyAfterPrune =
            preflight.reason == ManifoldFinalizeRejectReason::AllSeparatedAfterPrune ||
            preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold;
        return preflight;
    }

    const ManifoldPrunePreflight prunePreflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);
    preflight.needsPruning = prunePreflight.needs_pruning();
    preflight.wouldBeEmptyAfterPrune = prunePreflight.wouldBeEmpty;

    preflight.needsNormalNormalize = manifold.needsNormalNormalization(frictionEpsilon);
    preflight.canFinalize = true;
    preflight.canReuseFrictionBasis = can_skip_friction_basis_rebuild(manifold, frictionEpsilon);
    preflight.needsFrictionBasis = needs_friction_basis_refresh(manifold, frictionEpsilon);
    return preflight;
}

bool can_skip_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon)
                .can_finalize();
}

bool prune_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    const ManifoldPrunePreflight preflight =
        preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
    if (preflight.reason != ManifoldPruneRejectReason::None) {
        if (preflight.reason == ManifoldPruneRejectReason::AllSeparated) {
            manifold.clear();
        }
        return false;
    }
    if (preflight.can_skip_prune(shallowMinDepth)) {
        return !manifold.empty();
    }
    if (preflight.needs_shallow_pruning(shallowMinDepth)) {
        manifold.pruneShallowPenetrationsIfNeeded(shallowMinDepth);
    }
    return manifold.pruneContactPointsIfNeeded(separationEpsilon, duplicateEpsilon);
}

bool finalize_contact_manifold_with_preflight(
    ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    const ManifoldFinalizePreflight preflight =
        preflight_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
    if (!preflight.can_finalize()) {
        return false;
    }
    if (preflight.needsPruning) {
        if (!prune_contact_manifold_with_preflight(
                manifold, separationEpsilon, duplicateEpsilon)) {
            manifold.clear();
            return false;
        }
    }
    return generate_contact_manifold(manifold);
}

bool should_run_manifold_prune(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);
}

bool should_run_manifold_finalize(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 frictionEpsilon) {
    return !can_skip_manifold_finalize(manifold, separationEpsilon, duplicateEpsilon, frictionEpsilon);
}

bool can_prune_manifold_in_place(
    const ContactManifold& manifold,
    f32 separationEpsilon,
    f32 duplicateEpsilon,
    f32 shallowMinDepth) {
    return preflight_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)
        .can_prune_in_place();
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

// --- deepen additive from deepen-b4-narrowphase-guards-56bb ---
ManifoldPrunePreflight preflight_manifold_prune_ex(
bool can_skip_manifold_prune(const ManifoldPrunePreflight& preflight) {
bool ContactManifold::pruneFromPreflight(
    const ManifoldPrunePreflight& preflight,

// --- deepen additive from b4-narrowphase-deepen-guards-e063 ---
    preflight.needsPrune = prunePreflight.needs_pruning();
bool should_skip_manifold_finalize(
    if (should_skip_manifold_finalize(manifold)) {

// --- deepen additive from b4-narrowphase-deepen-guards-c64f ---
ManifoldFinalizePreflight preflight_finalize_contact_manifold(
bool should_skip_finalize_contact_manifold(const ContactManifold& manifold) {
    const ManifoldFinalizePreflight preflight = preflight_finalize_contact_manifold(manifold);

// --- deepen additive from b4-narrowphase-guards-deepen-ea96 ---
ManifoldFinalizePreflight preflight_manifold_finalize(const ContactManifold& manifold) {
bool should_skip_manifold_finalize(const ContactManifold& manifold) {
bool can_finalize_with_preflight(const ManifoldFinalizePreflight& preflight) {

// --- deepen additive from deepen-b4-narrowphase-guards-5111 ---
    const ManifoldFinalizePreflight preflight = preflight_manifold_finalize(manifold);

// --- deepen additive from b4-narrowphase-deepen-guards-a773 ---
    case ManifoldPruneRejectReason::WouldBeEmptyAfterPrune:
    case ManifoldPruneRejectReason::NoPruningNeeded:
        return ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
        return ManifoldPruneRejectReason::NoPruningNeeded;
    if (preflight.reason == ManifoldPruneRejectReason::EmptyManifold) {
    preflight.wouldBeEmpty = preflight.reason == ManifoldPruneRejectReason::WouldBeEmptyAfterPrune;
    case ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune:
        return ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune;
    if (preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold) {

// --- deepen additive from deepen-b4-narrowphase-guards-72f5 ---
        preflight.reason = ManifoldPruneRejectReason::EmptyManifold;
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);

// --- deepen additive from deepen-b4-narrowphase-guards-c9f2 ---
    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon)) {

// --- deepen additive from deepen-b4-narrowphase-guards-d130 ---
    case ManifoldPruneRejectReason::Empty:
    case ManifoldPruneRejectReason::WouldBeEmpty:
        return ManifoldPruneRejectReason::Empty;
        return ManifoldPruneRejectReason::WouldBeEmpty;
    return !should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon);

// --- deepen additive from deepen-b4-narrowphase-guards-1468 ---
ManifoldFinalizeDeepenPreflight preflight_manifold_finalize_deepen(
    ManifoldFinalizeDeepenPreflight preflight{};
    const ManifoldFinalizePreflight basePreflight =
    preflight.canFinalize = basePreflight.canFinalize;
    preflight.needsPruning = basePreflight.needsPruning;
    preflight.wouldBeEmptyAfterPrune = basePreflight.wouldBeEmptyAfterPrune;
    preflight.needsFrictionBasis = basePreflight.needsFrictionBasis;
    preflight.canReuseFrictionBasis = basePreflight.canReuseFrictionBasis;

// --- deepen additive from deepen-b4-narrowphase-guards-1764 ---
    if (preflight.skipped || preflight.reason != ManifoldPruneRejectReason::None) {

// --- deepen additive from b4-narrowphase-deepen-guards-6e88 ---
ManifoldGeneratePreflight preflight_generate_contact_manifold(
    ManifoldGeneratePreflight preflight{};
ManifoldPruneChainPreflight preflight_manifold_prune_chain(
    ManifoldPruneChainPreflight preflight{};
ManifoldFinalizeChainPreflight preflight_manifold_finalize_chain(
    ManifoldFinalizeChainPreflight preflight{};

// --- deepen additive from deepen-b4-narrowphase-guards-ddb5 ---
    case ManifoldFinalizeRejectReason::Empty:
    case ManifoldFinalizeRejectReason::EmptyAfterPrune:
        return ManifoldFinalizeRejectReason::Empty;
        return ManifoldFinalizeRejectReason::EmptyAfterPrune;
    preflight.rejectReason = ManifoldFinalizeRejectReason::None;

// --- deepen additive from deepen-b4-narrowphase-guards-f4c2 ---
    case ManifoldFinalizeRejectReason::NoPenetration:
        return ManifoldFinalizeRejectReason::NoPenetration;
ManifoldPruneDispatchPreflight preflight_manifold_prune_dispatch(
    ManifoldPruneDispatchPreflight preflight{};
    const ManifoldPrunePreflight regularPreflight =
    preflight.needsRegularPrune = regularPreflight.needs_pruning();
    preflight.wouldBeEmpty = regularPreflight.wouldBeEmpty;
    const ManifoldPruneDispatchPreflight preflight =

// --- deepen additive from deepen-b4-narrowphase-guards-0339 ---
    case ManifoldPruneRejectReason::AlreadyClean:
        return ManifoldPruneRejectReason::AlreadyClean;
    if (preflight.reason == ManifoldPruneRejectReason::Empty) {
    if (should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth)) {
    if (prunePreflight.wouldBeEmpty) {
    if (preflight.reason == ManifoldFinalizeRejectReason::Empty) {

// --- deepen additive from b4-narrowphase-deepen-guards-6242 ---
    case ManifoldPruneRejectReason::NoPruneNeeded:
        return ManifoldPruneRejectReason::NoPruneNeeded;
    const ManifoldPruneRejectReason reason =
    if (reason == ManifoldPruneRejectReason::EmptyManifold ||
        reason == ManifoldPruneRejectReason::WouldBeEmptyAfterPrune) {
    if (reason == ManifoldPruneRejectReason::NoPruneNeeded) {

// --- deepen additive from deepen-b4-narrowphase-guards-071f ---
    if (should_skip_manifold_prune(*this, separationEpsilon, duplicateEpsilon)) {
        preflight.reason = ManifoldPruneRejectReason::AlreadyClean;
        preflight.skipped = preflight.reason == ManifoldFinalizeRejectReason::EmptyManifold;
        if (preflight.reason == ManifoldFinalizeRejectReason::WouldBeEmptyAfterPrune) {

// --- deepen additive from deepen-narrowphase-guards-6b0c ---
    case ManifoldPruneRejectReason::NothingToPrune:
        return ManifoldPruneRejectReason::NothingToPrune;

// --- deepen additive from deepen-b4-narrowphase-guards-fbc2 ---
    return should_skip_manifold_prune(manifold, separationEpsilon, duplicateEpsilon, shallowMinDepth);

// --- deepen additive from deepen-b4-narrowphase-guard-pass-9852 ---
    case ManifoldPruneRejectReason::CleanManifold:
    if (prunePreflight.can_skip_prune(shallowMinDepth)) {
        return ManifoldPruneRejectReason::CleanManifold;

// --- deepen additive from b4-narrowphase-deepen-guards-046d ---
ManifoldPruneFinalizePreflight preflight_manifold_prune_finalize(
    ManifoldPruneFinalizePreflight preflight{};
bool should_skip_manifold_prune_finalize(
