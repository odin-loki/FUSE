#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

namespace {

bool contact_body_in_range(u32 bodyCount, u32 bodyIndex) {
    return bodyIndex < bodyCount;
}

bool contact_pair_in_range(u32 bodyCount, const narrowphase::ContactManifold& contact) {
    return contact_body_in_range(bodyCount, contact.bodyA) && contact_body_in_range(bodyCount, contact.bodyB);
}

bool distance_pair_in_range(u32 bodyCount, const DistanceConstraint& constraint) {
    return contact_body_in_range(bodyCount, constraint.bodyA) && contact_body_in_range(bodyCount, constraint.bodyB);
}

} // namespace

const char* island_build_reject_reason_name(IslandBuildRejectReason reason) {
    switch (reason) {
    case IslandBuildRejectReason::None:
        return "None";
    case IslandBuildRejectReason::OutOfRangeContactBody:
        return "OutOfRangeContactBody";
    case IslandBuildRejectReason::OutOfRangeDistanceBody:
        return "OutOfRangeDistanceBody";
    }
    return "Unknown";
}

bool has_out_of_range_contact_body(u32 bodyCount, const std::vector<narrowphase::ContactManifold>& contacts) {
    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact_pair_in_range(bodyCount, contact)) {
            return true;
        }
    }
    return false;
}

bool has_out_of_range_distance_body(u32 bodyCount, const std::vector<DistanceConstraint>& distanceConstraints) {
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_pair_in_range(bodyCount, constraint)) {
            return true;
        }
    }
    return false;
}

IslandBuildRejectReason island_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    if (has_out_of_range_contact_body(bodyCount, contacts)) {
        return IslandBuildRejectReason::OutOfRangeContactBody;
    }
    if (has_out_of_range_distance_body(bodyCount, distanceConstraints)) {
        return IslandBuildRejectReason::OutOfRangeDistanceBody;
    }
    return IslandBuildRejectReason::None;
}

bool island_build_rejects_for_reason(u32 bodyCount,
                                     const std::vector<narrowphase::ContactManifold>& contacts,
                                     const std::vector<DistanceConstraint>& distanceConstraints,
                                     IslandBuildRejectReason expected) {
    return island_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
}

IslandBuildPreflight preflight_island_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandBuildPreflight preflight{};
    preflight.bodyCount = bodyCount;
    preflight.contactCount = static_cast<u32>(contacts.size());
    preflight.distanceCount = static_cast<u32>(distanceConstraints.size());
    preflight.reason = island_build_reject_reason(bodyCount, contacts, distanceConstraints);

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (!contact_pair_in_range(bodyCount, contact)) {
            ++preflight.outOfRangeContactCount;
        }
    }
    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!distance_pair_in_range(bodyCount, constraint)) {
            ++preflight.outOfRangeDistanceCount;
        }
    }

    preflight.skipped = preflight.reason != IslandBuildRejectReason::None;
    return preflight;
}

bool should_skip_island_build(u32 bodyCount,
                              const std::vector<narrowphase::ContactManifold>& contacts,
                              const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_build(bodyCount, contacts, distanceConstraints).can_build();
}

bool build_guarded(ContactIslandGraph& graph,
                   u32 bodyCount,
                   const std::vector<narrowphase::ContactManifold>& contacts,
                   const std::vector<DistanceConstraint>& distanceConstraints) {
    if (should_skip_island_build(bodyCount, contacts, distanceConstraints)) {
        return false;
    }
    graph.build(bodyCount, contacts, distanceConstraints);
    return true;
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
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
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
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);
        }
    }

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
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
