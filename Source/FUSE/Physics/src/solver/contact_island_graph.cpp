#include <fuse/physics/solver/contact_island_graph.hpp>

#include <algorithm>

namespace fuse::physics {

IslandGraphBuildPreflight preflight_island_graph_build(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildPreflight preflight{};
    preflight.stats.bodyCount = bodyCount;
    preflight.stats.contactSlotCount = static_cast<u32>(contacts.size());
    preflight.stats.distanceSlotCount = static_cast<u32>(distanceConstraints.size());

    for (const narrowphase::ContactManifold& contact : contacts) {
        if (contact.valid) {
            ++preflight.stats.validContactCount;
        }
        const bool inRange = contact.bodyA < bodyCount && contact.bodyB < bodyCount;
        if (inRange) {
            ++preflight.stats.inRangeContactCount;
        } else if (contact.valid) {
            ++preflight.stats.outOfRangeContactBodyCount;
        }
    }

    for (const DistanceConstraint& constraint : distanceConstraints) {
        if (constraint.bodyA < bodyCount && constraint.bodyB < bodyCount) {
            ++preflight.stats.inRangeDistanceCount;
        } else {
            ++preflight.stats.outOfRangeDistanceBodyCount;
        }
    }

    preflight.skipped = bodyCount == 0u && preflight.stats.inRangeContactCount == 0u &&
                        preflight.stats.inRangeDistanceCount == 0u;
    return preflight;
}

bool should_skip_island_graph_build(u32 bodyCount,
                                    const std::vector<narrowphase::ContactManifold>& contacts,
                                    const std::vector<DistanceConstraint>& distanceConstraints) {
    return !preflight_island_graph_build(bodyCount, contacts, distanceConstraints).can_build();
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

IslandGraphBuildOutcome ContactIslandGraph::build_guarded(
    u32 bodyCount,
    const std::vector<narrowphase::ContactManifold>& contacts,
    const std::vector<DistanceConstraint>& distanceConstraints) {
    IslandGraphBuildOutcome outcome{};
    const IslandGraphBuildPreflight preflight =
        preflight_island_graph_build(bodyCount, contacts, distanceConstraints);
    outcome.unsafeRefs = preflight.has_unsafe_refs();
    if (!preflight.can_build()) {
        clear();
        outcome.skipped = true;
        return outcome;
    }

    build(bodyCount, contacts, distanceConstraints);
    outcome.built = true;
    return outcome;
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
