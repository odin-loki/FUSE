#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

namespace {

bool contactBodiesInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

ContactIslandGraphBuildRejectReason diagnoseBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    u32 inRangeContactCount = 0;
    u32 inRangeDistanceCount = 0;
    bool outOfRangeContact = false;
    bool outOfRangeDistance = false;

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++inRangeContactCount;
        } else {
            outOfRangeContact = true;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++inRangeDistanceCount;
        } else {
            outOfRangeDistance = true;
        }
    }

    if (bodyCount == 0u && inRangeContactCount == 0u && inRangeDistanceCount == 0u) {
        return ContactIslandGraphBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContact) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies;
    }
    if (outOfRangeDistance) {
        return ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies;
    }
    return ContactIslandGraphBuildRejectReason::None;
}

} // namespace

const char* contactIslandGraphBuildRejectReasonName(ContactIslandGraphBuildRejectReason reason) {
    switch (reason) {
    case ContactIslandGraphBuildRejectReason::None:
        return "None";
    case ContactIslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case ContactIslandGraphBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case ContactIslandGraphBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";
    }
}

ContactIslandGraphBuildRejectReason contactIslandGraphBuildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
}

bool contactIslandGraphBuildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    ContactIslandGraphBuildRejectReason expected) {
    return contactIslandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraphBuildPreflight preflightContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    ContactIslandGraphBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.validContactCount;
        }
        if (contact.valid && contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            ++preflight.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            ++preflight.inRangeDistanceCount;
        } else {
            ++preflight.outOfRangeDistanceBodyCount;
        }
    }

    preflight.reason = diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
    preflight.skipped = preflight.reason == ContactIslandGraphBuildRejectReason::EmptyInput;
    return preflight;
}

bool canSkipContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
}

bool shouldRunContactIslandGraphBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return preflightContactIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();
}

void ContactIslandGraph::clear() {
    parent_.clear();
    islands_.clear();
}

u32 ContactIslandGraph::findRoot(u32 index) const {
    u32 root = index;
    while (parent_[root] != root) {
        root = parent_[root];
    }
    return root;
}

void ContactIslandGraph::compressPath(u32 index) {
    const u32 root = findRoot(index);
    while (parent_[index] != root) {
        const u32 next = parent_[index];
        parent_[index] = root;
        index = next;
    }
}

void ContactIslandGraph::unionBodies(u32 a, u32 b) {
    if (a >= parent_.size() || b >= parent_.size()) {
        return;
    }

    u32 rootA = findRoot(a);
    u32 rootB = findRoot(b);
    if (rootA == rootB) {
        return;
    }
    if (rootA < rootB) {
        parent_[rootB] = rootA;
    } else {
        parent_[rootA] = rootB;
    }
}

void ContactIslandGraph::build(u32 bodyCount,
                               const std::vector<narrowphase::ContactManifold>& contacts,
                               const std::vector<DistanceConstraint>& distanceConstraints) {
    clear();
    parent_.resize(bodyCount);
    for (u32 i = 0; i < bodyCount; ++i) {
        parent_[i] = i;
    }

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact.valid) {
            continue;
        }
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
        unionBodies(constraint.bodyA, constraint.bodyB);
    }

    for (u32 i = 0; i < bodyCount; ++i) {
        compressPath(i);
    }

    std::vector<u32> rootToIsland(bodyCount, invalidIsland);
    islands_.clear();

    for (u32 bodyIndex = 0; bodyIndex < bodyCount; ++bodyIndex) {
        const u32 root = findRoot(bodyIndex);
        if (rootToIsland[root] == invalidIsland) {
            rootToIsland[root] = static_cast<u32>(islands_.size());
            islands_.push_back({});
        }
        islands_[rootToIsland[root]].bodyIndices.push_back(bodyIndex);
    }

    for (u32 contactIndex = 0; contactIndex < contacts.size(); ++contactIndex) {
        const narrowphase::ContactManifold& contact = contacts[contactIndex];
        if (!contact.valid) {
            continue;
        }
        if (!contactBodiesInRange(contact.bodyA, contact.bodyB, bodyCount)) {
            continue;
        }
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);
        }
    }

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (!contactBodiesInRange(constraint.bodyA, constraint.bodyB, bodyCount)) {
            continue;
        }
        const u32 islandIndex = rootToIsland[findRoot(constraint.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].distanceIndices.push_back(distanceIndex);
        }
    }

    std::sort(islands_.begin(), islands_.end(), [](const Island& left, const Island& right) {
        if (left.bodyIndices.empty() || right.bodyIndices.empty()) {
            return left.bodyIndices.size() < right.bodyIndices.size();
        }
        return left.bodyIndices.front() < right.bodyIndices.front();
    });
}

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    if (canSkipContactIslandGraphBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
    return true;
}

u32 ContactIslandGraph::constrainedIslandCount() const {
    u32 count = 0;
    for (const Island& island : islands_) {
        if (!island.isEmpty()) {
            ++count;
        }
    }
    return count;
}

u32 ContactIslandGraph::bodyIsland(u32 bodyIndex) const {
    if (bodyIndex >= parent_.size()) {
        return invalidIsland;
    }

    const u32 root = findRoot(bodyIndex);
    for (u32 islandIndex = 0; islandIndex < islands_.size(); ++islandIndex) {
        for (u32 index : islands_[islandIndex].bodyIndices) {
            if (index == bodyIndex || findRoot(index) == root) {
                return islandIndex;
            }
        }
    }
    return invalidIsland;
}

} // namespace fuse::physics

// --- deepen additive from deepen-pbd-island-guards-faad ---
const char* islandBuildRejectReasonName(IslandBuildRejectReason reason) {
    case IslandBuildRejectReason::None:
    case IslandBuildRejectReason::SelfPair:
    case IslandBuildRejectReason::OutOfRangeBody:
    case IslandBuildRejectReason::InvalidContact:
IslandBuildPreflight preflight_island_build(u32 bodyCount,
    IslandBuildPreflight preflight{};
        const IslandBuildRejectReason reason = contactBuildRejectReason(contact, bodyCount);
        const IslandBuildRejectReason reason = distanceBuildRejectReason(constraint, bodyCount);
bool should_skip_island_build(u32 bodyCount,

// --- deepen additive from pbd-island-sleep-build-preflights-cb2c ---
    const IslandBuildPreflight preflight = preflight_island_build(bodyCount, contacts, distanceConstraints);
bool should_skip_island_build(u32 bodyCount) {

// --- deepen additive from deepen-pbd-island-guards-a261 ---
IslandBuildPreflight preflight_island_graph_build(
bool should_skip_island_graph_build(u32 bodyCount) {

// --- deepen additive from deepen-pbd-island-sleep-build-guards-9e33 ---
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {

// --- deepen additive from pbd-island-guards-deepen-5934 ---
    if (should_skip_island_build(bodyCount)) {

// --- deepen additive from deepen-pbd-island-guards-ac8e ---
const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    case IslandBuildRejectReason::EmptyBodyCount:
    case IslandBuildRejectReason::InvalidContactBodyIndex:
    case IslandBuildRejectReason::InvalidDistanceBodyIndex:
IslandBuildRejectReason island_build_reject_reason(
        return IslandBuildRejectReason::EmptyBodyCount;
            return IslandBuildRejectReason::InvalidContactBodyIndex;
            return IslandBuildRejectReason::InvalidDistanceBodyIndex;
    return IslandBuildRejectReason::None;
    IslandBuildRejectReason expected) {
    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;

// --- deepen additive from deepen-pbd-island-build-sleep-wake-cc0e ---
        preflight.reason = IslandBuildRejectReason::EmptyBodyCount;

// --- deepen additive from deepen-pbd-island-guards-fd7c ---
    case IslandBuildRejectReason::ZeroBodies:
    case IslandBuildRejectReason::NoConstraints:
        return IslandBuildRejectReason::ZeroBodies;
        return IslandBuildRejectReason::NoConstraints;
    preflight.zeroBodies = preflight.reason == IslandBuildRejectReason::ZeroBodies;
    preflight.noConstraints = preflight.reason == IslandBuildRejectReason::NoConstraints;

// --- deepen additive from deepen-pbd-island-guards-3045 ---
    case IslandBuildRejectReason::OutOfRangeContactBody:
    case IslandBuildRejectReason::OutOfRangeDistanceBody:
        return IslandBuildRejectReason::OutOfRangeContactBody;
        return IslandBuildRejectReason::OutOfRangeDistanceBody;

// --- deepen additive from deepen-pbd-island-guards-a489 ---
    case IslandBuildRejectReason::ZeroBodyCount:
    case IslandBuildRejectReason::StaleContactBodyRefs:
    case IslandBuildRejectReason::StaleDistanceBodyRefs:
        preflight.reason = IslandBuildRejectReason::ZeroBodyCount;
        preflight.reason = IslandBuildRejectReason::StaleContactBodyRefs;
        preflight.reason = IslandBuildRejectReason::StaleDistanceBodyRefs;

// --- deepen additive from pbd-island-guards-deepen-77cf ---
    case IslandBuildRejectReason::OutOfRangeContactBodies:
    case IslandBuildRejectReason::OutOfRangeDistanceBodies:
        return IslandBuildRejectReason::ZeroBodyCount;
            return IslandBuildRejectReason::OutOfRangeContactBodies;
            return IslandBuildRejectReason::OutOfRangeDistanceBodies;

// --- deepen additive from deepen-pbd-island-guards-2105 ---
        preflight.reason = IslandBuildRejectReason::ZeroBodies;
        preflight.reason = IslandBuildRejectReason::OutOfRangeBodies;
    const IslandBuildPreflight preflight = preflight_island_graph_build(bodyCount, contacts, distanceConstraints);

// --- deepen additive from deepen-pbd-island-guards-358e ---
        preflight.reason = IslandBuildRejectReason::AllConstraintsStale;

// --- deepen additive from pbd-island-sleep-wake-preflights-1600 ---
        return IslandBuildRejectReason::OutOfRangeBodyRef;
    preflight.rejected = preflight.reason != IslandBuildRejectReason::None;

// --- deepen additive from deepen-pbd-island-sleep-build-guards-e836 ---
                                            const IslandBuildPreflight& inputPreflight) {
    stats.orphanContactCount = inputPreflight.outOfRangeContactCount + inputPreflight.invalidContactCount;
    stats.orphanDistanceCount = inputPreflight.outOfRangeDistanceCount;

// --- deepen additive from deepen-pbd-island-guards-9f8d ---
    case ContactIslandGraphBuildRejectReason::UnsafeRefs:
    case ContactIslandGraphBuildRejectReason::SelfContact:
        preflight.reason = ContactIslandGraphBuildRejectReason::EmptyInput;
        preflight.reason = ContactIslandGraphBuildRejectReason::UnsafeRefs;
        preflight.reason = ContactIslandGraphBuildRejectReason::SelfContact;

// --- deepen additive from deepen-pbd-island-guards-73b7 ---
    case IslandBuildRejectReason::EmptyInput:
    case IslandBuildRejectReason::UnsafeRefs:
        return IslandBuildRejectReason::EmptyInput;
        return IslandBuildRejectReason::UnsafeRefs;
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) != IslandBuildRejectReason::None;

// --- deepen additive from deepen-pbd-island-guards-e84e ---
    if (should_skip_island_graph_build(bodyCount, contacts, distanceConstraints)) {
IslandGraphBuildPreflight preflight_island_graph_build(
bool should_skip_island_graph_build(u32 bodyCount,
IslandGraphIntegrityPreflight preflight_island_graph_integrity(const ContactIslandGraph& graph,
    IslandGraphIntegrityPreflight preflight{};
bool should_skip_island_graph_integrity(const ContactIslandGraph& graph,

// --- deepen additive from deepen-pbd-island-guards-a022 ---
IslandGraphBuildPreflight preflightIslandGraphBuild(
    return !preflightIslandGraphBuild(bodyCount, contacts, distanceConstraints).can_build();

// --- deepen additive from deepen-pbd-island-guards-79a3 ---
IslandGraphBuildPreflight preflight_graph_build(
bool should_skip_graph_build(u32 bodyCount,
    const IslandGraphBuildPreflight preflight = preflight_graph_build(bodyCount, contacts, distanceConstraints);

// --- deepen additive from deepen-b4-pbd-island-guards-0323 ---
ContactIslandBuildPreflight preflight_contact_island_build(
    ContactIslandBuildPreflight preflight{};
bool should_skip_contact_island_build(u32 bodyCount,
    if (should_skip_contact_island_build(bodyCount, contacts, distanceConstraints)) {

// --- deepen additive from deepen-pbd-island-b4-guards-e0ed ---
IslandGraphBuildPreflight preflight_contact_island_graph_build(
bool should_skip_contact_island_graph_build(
    if (should_skip_contact_island_graph_build(bodyCount, contacts, distanceConstraints)) {

// --- deepen additive from deepen-b4-pbd-island-preflights-fd1e ---
    return islandUnionRejectReason(bodyCount, bodyA, bodyB) == IslandUnionRejectReason::None;
const char* islandUnionRejectReasonName(IslandUnionRejectReason reason) {
    case IslandUnionRejectReason::None:
    case IslandUnionRejectReason::OutOfRangeBodyA:
    case IslandUnionRejectReason::OutOfRangeBodyB:
IslandUnionRejectReason islandUnionRejectReason(u32 bodyCount, u32 bodyA, u32 bodyB) {
        return IslandUnionRejectReason::OutOfRangeBodyA;
        return IslandUnionRejectReason::OutOfRangeBodyB;
    return IslandUnionRejectReason::None;
bool islandUnionRejectsForReason(u32 bodyCount, u32 bodyA, u32 bodyB, IslandUnionRejectReason expected) {
    return islandUnionRejectReason(bodyCount, bodyA, bodyB) == expected;

// --- deepen additive from deepen-pbd-island-guards-6fff ---
IslandGraphBuildPreflight ContactIslandGraph::preflightBuildInputs(
    return !preflightBuildInputs(bodyCount, contacts, distanceConstraints).can_build();

// --- deepen additive from deepen-b4-pbd-island-guards-9fc4 ---
const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    case IslandGraphBuildRejectReason::None:
    case IslandGraphBuildRejectReason::EmptyInputs:
    case IslandGraphBuildRejectReason::OutOfRangeContactBody:
    case IslandGraphBuildRejectReason::OutOfRangeDistanceBody:
IslandGraphBuildRejectReason island_graph_build_reject_reason(
        return IslandGraphBuildRejectReason::EmptyInputs;
            return IslandGraphBuildRejectReason::OutOfRangeContactBody;
            return IslandGraphBuildRejectReason::OutOfRangeDistanceBody;
    return IslandGraphBuildRejectReason::None;

// --- deepen additive from deepen-pbd-island-preflights-e740 ---
    case IslandGraphBuildRejectReason::EmptyInput:
    case IslandGraphBuildRejectReason::UnsafeContactRefs:
    case IslandGraphBuildRejectReason::UnsafeDistanceRefs:
        return IslandGraphBuildRejectReason::EmptyInput;
            return IslandGraphBuildRejectReason::UnsafeContactRefs;
            return IslandGraphBuildRejectReason::UnsafeDistanceRefs;

// --- deepen additive from deepen-b4-pbd-island-guards-8369 ---
const char* ContactIslandGraph::buildRejectReasonName(BuildRejectReason reason) {
    case BuildRejectReason::None:
    case BuildRejectReason::EmptyInputs:
    case BuildRejectReason::OutOfRangeContactBodies:
    case BuildRejectReason::OutOfRangeDistanceBodies:
ContactIslandGraph::BuildRejectReason ContactIslandGraph::buildRejectReason(
    const BuildPreflight preflight = preflightBuild(bodyCount, contacts, distanceConstraints);
    return buildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
ContactIslandGraph::BuildPreflight ContactIslandGraph::preflightBuild(
        preflight.reason = BuildRejectReason::EmptyInputs;
        preflight.reason = BuildRejectReason::OutOfRangeContactBodies;
        preflight.reason = BuildRejectReason::OutOfRangeDistanceBodies;
    return !preflightBuild(bodyCount, contacts, distanceConstraints).can_build();

// --- deepen additive from deepen-pbd-island-pipeline-guards-9e7f ---
    case IslandBuildRejectReason::EmptyInputs:
    case IslandBuildRejectReason::OutOfRangeContact:
    case IslandBuildRejectReason::OutOfRangeDistance:
            return IslandBuildRejectReason::OutOfRangeContact;
            return IslandBuildRejectReason::OutOfRangeDistance;
        return IslandBuildRejectReason::EmptyInputs;
ContactIslandGraphBuildPreflight preflight_contact_island_graph_build(
    preflight.skipped = preflight.reason == IslandBuildRejectReason::EmptyInputs;

// --- deepen additive from deepen-pbd-island-pipeline-guards-6b7f ---
    case IslandGraphBuildRejectReason::UnsafeContactRef:
    case IslandGraphBuildRejectReason::UnsafeDistanceRef:
        return IslandGraphBuildRejectReason::UnsafeContactRef;
        return IslandGraphBuildRejectReason::UnsafeDistanceRef;
    preflight.skipped = preflight.reason == IslandGraphBuildRejectReason::EmptyInput;

// --- deepen additive from deepen-pbd-island-reject-reasons-b344 ---
const char* islandGraphBuildRejectReasonName(IslandGraphBuildRejectReason reason) {
    case IslandGraphBuildRejectReason::OutOfRangeRefs:
IslandGraphBuildRejectReason islandGraphBuildRejectReason(
        return IslandGraphBuildRejectReason::OutOfRangeRefs;
    return islandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
    if (islandGraphBuildRejectReason(bodyCount, contacts, distanceConstraints) !=
        IslandGraphBuildRejectReason::None) {
