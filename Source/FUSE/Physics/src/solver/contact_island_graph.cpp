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
