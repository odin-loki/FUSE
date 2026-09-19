#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

namespace {

bool contactBodiesInRange(u32 bodyCount, u32 bodyA, u32 bodyB) {
    return bodyA < bodyCount && bodyB < bodyCount;
}

} // namespace

const char* ContactIslandGraph::buildRejectReasonName(BuildRejectReason reason) {
    switch (reason) {
    case BuildRejectReason::None:
        return "None";
    case BuildRejectReason::EmptyInputs:
        return "EmptyInputs";
    case BuildRejectReason::OutOfRangeContactBodies:
        return "OutOfRangeContactBodies";
    case BuildRejectReason::OutOfRangeDistanceBodies:
        return "OutOfRangeDistanceBodies";
    default:
        return "Unknown";
    }
}

ContactIslandGraph::BuildRejectReason ContactIslandGraph::buildRejectReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    const BuildPreflight preflight = preflightBuild(bodyCount, contacts, distanceConstraints);
    return preflight.reason;
}

bool ContactIslandGraph::buildRejectsForReason(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints,
    BuildRejectReason expected) {
    return buildRejectReason(bodyCount, contacts, distanceConstraints) == expected;
}

ContactIslandGraph::BuildPreflight ContactIslandGraph::preflightBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    BuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        if (contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.skippedOutOfRangeContactCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.skippedOutOfRangeDistanceCount;
        }
    }

    if (bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
        preflight.stats.inRangeDistanceCount == 0u) {
        preflight.reason = BuildRejectReason::EmptyInputs;
        preflight.rejected = true;
        return preflight;
    }

    if (preflight.stats.skippedOutOfRangeContactCount > 0u) {
        preflight.reason = BuildRejectReason::OutOfRangeContactBodies;
        preflight.rejected = true;
        return preflight;
    }

    if (preflight.stats.skippedOutOfRangeDistanceCount > 0u) {
        preflight.reason = BuildRejectReason::OutOfRangeDistanceBodies;
        preflight.rejected = true;
        return preflight;
    }

    return preflight;
}

bool ContactIslandGraph::shouldSkipBuild(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflightBuild(bodyCount, contacts, distanceConstraints).can_build();
}

bool ContactIslandGraph::buildGuarded(u32 bodyCount,
                                      const std::vector<narrowphase::ContactManifold>& contacts,
                                      const std::vector<DistanceConstraint>& distanceConstraints) {
    if (shouldSkipBuild(bodyCount, contacts, distanceConstraints)) {
        clear();
        return false;
    }
    build(bodyCount, contacts, distanceConstraints);
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
        if (!contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
            continue;
        }
        unionBodies(contact.bodyA, contact.bodyB);
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (!contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
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
        if (!contactBodiesInRange(bodyCount, contact.bodyA, contact.bodyB)) {
            continue;
        }
        const u32 islandIndex = rootToIsland[findRoot(contact.bodyA)];
        if (islandIndex != invalidIsland) {
            islands_[islandIndex].contactIndices.push_back(contactIndex);
        }
    }

    for (u32 distanceIndex = 0; distanceIndex < distanceConstraints.size(); ++distanceIndex) {
        const DistanceConstraint& constraint = distanceConstraints[distanceIndex];
        if (!contactBodiesInRange(bodyCount, constraint.bodyA, constraint.bodyB)) {
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
