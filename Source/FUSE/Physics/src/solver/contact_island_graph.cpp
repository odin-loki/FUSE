#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

namespace {

bool contactBodiesInRange(u32 bodyA, u32 bodyB, u32 bodyCount) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

IslandGraphBuildRejectReason diagnoseBuildRejectReason(
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
        return IslandGraphBuildRejectReason::EmptyInput;
    }
    if (outOfRangeContact) {
        return IslandGraphBuildRejectReason::OutOfRangeContactBodies;
    }
    if (outOfRangeDistance) {
        return IslandGraphBuildRejectReason::OutOfRangeDistanceBodies;
    }
    return IslandGraphBuildRejectReason::None;
}

} // namespace

const char* island_graph_build_reject_reason_name(IslandGraphBuildRejectReason reason) {
    switch (reason) {
    case IslandGraphBuildRejectReason::None:
        return "None";
    case IslandGraphBuildRejectReason::EmptyInput:
        return "EmptyInput";
    case IslandGraphBuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case IslandGraphBuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";
    }
}

IslandGraphBuildRejectReason island_graph_build_reject_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return diagnoseBuildRejectReason(bodyCount, contacts, distanceConstraints);
}

bool island_graph_build_rejects_for_reason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    IslandGraphBuildRejectReason expected) {
    return island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) == expected;
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

bool ContactIslandGraph::build_guarded(u32 bodyCount,
                                       const std::vector<narrowphase::ContactManifold>& contacts,
                                       const std::vector<DistanceConstraint>& distanceConstraints) {
    if (island_graph_build_reject_reason(bodyCount, contacts, distanceConstraints) !=
        IslandGraphBuildRejectReason::None) {
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
